//==============================================================================
// gui_win.cpp — 中文界面（Windows / Win32 实现 VST3 IPlugView）
//
// 对应 macOS 的 gui.mm。设计保持一致：
//   - 波形预览 + 可拖动播放头
//   - 带符号起始偏移（在波形上按住 Ctrl / Alt 拖动）
//   - 小节网格（BPM + 拍号）
//   - 播放状态灯、音量、回到播放头、使用指南
//   - 拖入音频文件 / 点按钮选文件
//
// 与宿主的关系：VST3 在 Windows 上通过 kPlatformTypeHWND 传父窗口句柄，
//   这里创建一个子窗口挂上去，控件全部是它的子窗口。
//==============================================================================
#include "gui.h"
#include "crashguard.h"   // ap::crashLog：跟随疑似失效时落一条日志
#include "ui_layout.h"
#include "numeric_input.h"
#include "ui_theme.h"
#include "scorebridge.h"
#include "video_timeline.h"
#include "updater.h"
#include "video_view.h"

#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <objbase.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <memory>
#include <string>
#include <stdexcept>
#include <vector>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shell32.lib")

// Win32 的鼠标按键掩码里【没有 Alt 位】——只有 MK_CONTROL / MK_SHIFT /
// MK_LBUTTON 等，并不存在 MK_ALT（MSVC 会报 C2065 undeclared identifier）。
// 这里自定义一个不与系统冲突的位，由 modsNow() 用 GetKeyState(VK_MENU) 填充。
#define AP_MK_ALT 0x8000

// WM_DPICHANGED / WM_MOUSEHWHEEL 在较老的 SDK / _WIN32_WINNT 下没有定义，兜一个。
// （WM_MOUSEHWHEEL 是 Vista 起才有的：横向滚轮 = 鼠标滚轮左右拨 / 触控板左右滑。）
#ifndef WM_DPICHANGED
#define WM_DPICHANGED 0x02E0
#endif
#ifndef WM_MOUSEHWHEEL
#define WM_MOUSEHWHEEL 0x020E
#endif

namespace {

//---- 面板尺寸（96 DPI 下的【设计值】，统一乘 m_scale 后才是物理像素）----------
//
// ⭐ 与旧版最大的区别：面板不再写死。布局按【实际客户区】自适应，宿主给多宽，
//    波形区就跟着多宽（音频工具最需要的就是横向分辨率：看得见鼓点，才谈得上对拍子）。
//    旧版写死 340×384，波形只有 320 px，用户实测「界面太窄」。
// ⚠️ 面板尺寸【由宿主决定】：MuseScore 4.x 会把编辑器窗口的 min/max 设成同一个值
//    （钉死），既不显示缩放边框、也从不调用 canResize() —— 别对外承诺「能拖窗口边缘」。
//
//   kPanelW/H    = 默认尺寸（800 x 520）
//   kPanel*Min   = 波形工具栏与底部两个参数区的布局下限。
//   kPanel*Max   = 上限。防止宿主报一个荒唐的尺寸（如 0 或 100000）把内存吃光。
const int kPanelW    = ap::ui::defaultWidth;
const int kPanelH    = ap::ui::defaultHeight;
const int kPanelWMin = ap::ui::minWidth;
const int kPanelHMin = ap::ui::minHeight;
const int kPanelWMax = 1700;
const int kPanelHMax = 1200;
const COLORREF kUpdateAlert = RGB (220, 55, 55);
const int kWaveW  = 720;      ///< 波形区最小宽（面板缩到下限时用它兜底）
const int kWaveH  = 150;      ///< 波形区最小高

// 拍号条：波形区【顶部】划出的一条窄带，按小节画「N/M」方块，点方块弹拍号设置。
// 条高从波形区内部让出（波形区窗口 frame 不变，免得连带动布局与验证器）。
const int kTsStripH = 22;     ///< 拍号条高（设计像素）
const int kTsBoxH   = 15;     ///< 方块高（设计像素）
const int kTsBoxGap = 4;      ///< 相邻方块最小间隔（太挤就跳过不画）

// 分家检测阈值（秒）：正常播放时「音频实际位置」与「谱面位置 + 偏移」只差一个
// 音频缓冲的量级（几十毫秒）。超过这个值就认为跟随出了问题，波形区右上角会亮
// 出红色错位徽标，提示用户点「回到播放头」一键修复。
// 与 macOS / Linux 端取同一个值，别各写各的。
const double kDivergenceWarnSec = 0.25;

//---- 「点完『回到播放头』仍然错位」的判定窗口 --------------------------------
// 正常点一下就对齐了。若这么久之内又错位，说明偏差不是一次能纠正的，而是某个
// 跟随条件**持续**为假 —— 再点按钮也没用，徽标文案升级为「建议重启插件」，
// 并记一条日志（带冷却，避免抖动着刷屏）。
// ⚠️ 这里只做「提示升级」，绝不自动 seek：0.25 秒的偏差等于十几个音频块，
//    属结构性故障，自动 seek 会下一块又错开 → 每几十毫秒拉一次 → 音频发抖。
//    （跳变场景的自动硬 seek 在 aplaysdk.cpp 的 captureHostTimeline 里。）
// 用 GetTickCount() 的毫秒计数：DWORD 无符号相减天然抗 49.7 天回绕。
const DWORD kFollowFailWindowMs = 3000;
const DWORD kFollowFailLogGapMs  = 5000;

//---- 视野自动跟随（与 macOS 端同一套规则，见 followPlayheadTo）--------------
// ① 用户手动操作（拖波形 / 缩放 / 滚轮 / 点缩放按钮）后暂停跟随 2 秒；
// ② 播放头远在画面之外（超一整个屏）→ 不追，绝不把用户的视野抢回来；
// ③ 唯一例外：相邻两帧的【谱面位置】差 > kHostJumpSec，即宿主自己跳转了
//    （点小节 / 循环回卷 / 拖播放头）—— 那种情况用户正等着画面跟过去。
const double kHostJumpSec       = 0.25;
const DWORD  kUserScrollHoldMs  = 2000;

//---- 新载入音频的默认视野 ----------------------------------------------------
// 不默认「整曲全览」：全览时波峰糊成一片，根本看不清鼓点，用户每次都得先手动
// 放大好几下（实测要按 11~12 次）。默认直接给 8 秒视野（≈4 小节 @120BPM 4/4），
// 一打开就能看清对齐状况。曲长不足 8 秒时仍回落到全览（见 afterLoad）。
// 三端同名同值（macOS 端在载入处用的就是这个值）。
const double kDefaultViewSpanSec = 8.0;

//---- 重绘节流 ----------------------------------------------------------------
// 拖动/滚轮时每收到一条鼠标消息就整块重绘，在虚拟机（软件 GDI + 抢占式调度）上
// 会把音频线程饿到出 crackle/变调。这里把「鼠标驱动的重绘」限到 ~28fps，
// 剩下的交给 50ms 的界面定时器兜底。
const DWORD kDragPaintGapMs = 36;

// 控件 ID
enum : int
{
    IDC_OPEN = 1001,
    IDC_ZOOM_OUT,
    IDC_ZOOM_IN,
    IDC_ZOOM_FIT,
    IDC_BACK_TO_SCORE,      // 按钮文案＝「回到播放头」（ID 保留原名，避免大范围改名）
    IDC_GRID_CHECK,
    IDC_NUM_CHECK,
    IDC_BPM_EDIT,
    IDC_BPM_UP,
    IDC_BPM_DOWN,
    IDC_TIMESIG_COMBO,
    IDC_VOLUME_SLIDER,
    IDC_OFFSET_ZERO,        // 「归零」：把起始偏移一键清零
    IDC_SEMITONE_EDIT,
    IDC_SEMITONE_UP,
    IDC_SEMITONE_DOWN,
    IDC_CENTS_EDIT,
    IDC_CENTS_UP,
    IDC_CENTS_DOWN,
    IDC_PITCH_RESET,
    IDC_LINK_SCORE,
    IDC_THEME_BTN,
    IDC_FOLLOW_SCORE,
    // ⚠️ 不能叫 IDC_HELP —— winuser.h 已经把 IDC_HELP 定义成「帮助光标」的资源 ID
    //    （MAKEINTRESOURCE(32651)）。同名枚举项会被宏展开成一串语法垃圾，
    //    MSVC 只报 "syntax error: missing '}' before '('"，很难看出是宏冲突。
    IDC_HELP_BTN,
    IDC_OFFSET_EDIT,
    IDC_RESERVED_VOLUME_EDIT,
    IDC_OFFSET_DOWN,
    IDC_OFFSET_UP,
    IDC_COLLAPSE,
    IDC_EXPAND,
    IDC_HELP_CLOSE,
    IDC_PROGRESS_SLIDER,
    IDC_UPDATE_CHECK,
    IDC_UPDATE_ACTION,
    IDC_OPEN_VIDEO,
    IDC_VIDEO_OPACITY,
    IDC_CLOSE_MEDIA,
    IDC_OPEN_VIDEO_LOCAL = 1401,
    IDC_OPEN_VIDEO_URL,
    IDC_TIMER_UI = 1200
};

const wchar_t* kMainClass = L"MuseToneMainView";
const wchar_t* kWaveClass = L"MuseToneWaveView";
const wchar_t* kFloatingClass = L"MuseToneFloatingView";

struct VideoUrlPrompt
{
    HWND window = nullptr;
    HWND edit = nullptr;
    HWND label = nullptr;
    HWND open = nullptr;
    HWND cancel = nullptr;
    HWND close = nullptr;
    ap::ui::Theme theme {};
    HFONT font = nullptr;
    HFONT titleFont = nullptr;
    HFONT iconFont = nullptr;
    HBRUSH background = nullptr;
    HBRUSH surface = nullptr;
    double scale = 1.0;
    std::wstring* value = nullptr;
    bool accepted = false;
    int px (int n) const { return static_cast<int> (std::lround (n * scale)); }
};

constexpr wchar_t kVideoUrlPromptClass[] = L"MuseSyncVideoUrlPrompt";

LRESULT CALLBACK videoUrlPromptProc (HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    auto* prompt = reinterpret_cast<VideoUrlPrompt*> (::GetWindowLongPtrW (hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE)
    {
        auto* create = reinterpret_cast<CREATESTRUCTW*> (lp);
        prompt = static_cast<VideoUrlPrompt*> (create->lpCreateParams);
        ::SetWindowLongPtrW (hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR> (prompt));
    }
    if (!prompt) return ::DefWindowProcW (hwnd, msg, wp, lp);
    const auto color = [] (unsigned rgb) { return RGB ((rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255); };
    const auto& theme = prompt->theme;
    switch (msg)
    {
        case WM_CREATE:
        {
            prompt->label = ::CreateWindowW (L"STATIC", L"视频网址",
                WS_CHILD | WS_VISIBLE, 24, 20, 512, 22, hwnd,
                nullptr, ::GetModuleHandleW (nullptr), nullptr);
            prompt->edit = ::CreateWindowExW (0, L"EDIT", L"",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, 24, 54, 512, 32, hwnd,
                reinterpret_cast<HMENU> (1), ::GetModuleHandleW (nullptr), nullptr);
            ::SendMessageW (prompt->edit, EM_SETCUEBANNER, FALSE,
                reinterpret_cast<LPARAM> (L"https://www.bilibili.com/video/..."));
            ::SendMessageW (prompt->edit, EM_SETLIMITTEXT, 2047, 0);
            prompt->open = ::CreateWindowW (L"BUTTON", L"打开", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                330, 160, 104, 36, hwnd, reinterpret_cast<HMENU> (2),
                ::GetModuleHandleW (nullptr), nullptr);
            prompt->cancel = ::CreateWindowW (L"BUTTON", L"取消", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                448, 160, 104, 36, hwnd, reinterpret_cast<HMENU> (3),
                ::GetModuleHandleW (nullptr), nullptr);
            prompt->close = ::CreateWindowW (L"BUTTON", L"关闭", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                0, 0, 32, 32, hwnd, reinterpret_cast<HMENU> (4), ::GetModuleHandleW (nullptr), nullptr);
            for (HWND control : {prompt->label, prompt->edit, prompt->open, prompt->cancel})
                ::SendMessageW (control, WM_SETFONT, reinterpret_cast<WPARAM> (prompt->font), TRUE);
            ::EnableWindow (prompt->open, FALSE);
            return 0;
        }
        case WM_SIZE:
        {
            const int width = LOWORD (lp);
            const int height = HIWORD (lp);
            const int margin = prompt->px (24);
            const int buttonWidth = prompt->px (88);
            const int buttonHeight = prompt->px (32);
            const int gap = prompt->px (12);
            const int buttonY = height - margin - buttonHeight;
            if (prompt->label) ::SetWindowPos (prompt->label, nullptr, margin, prompt->px (66),
                std::max (1, width - margin * 2), prompt->px (22), SWP_NOZORDER);
            if (prompt->edit) ::SetWindowPos (prompt->edit, nullptr, margin + prompt->px (10), prompt->px (102),
                std::max (1, width - margin * 2 - prompt->px (20)), prompt->px (24), SWP_NOZORDER);
            if (prompt->cancel) ::SetWindowPos (prompt->cancel, nullptr,
                width - margin - buttonWidth, buttonY, buttonWidth, buttonHeight, SWP_NOZORDER);
            if (prompt->open) ::SetWindowPos (prompt->open, nullptr,
                width - margin - buttonWidth * 2 - gap, buttonY, buttonWidth, buttonHeight, SWP_NOZORDER);
            if (prompt->close) ::SetWindowPos (prompt->close, nullptr,
                width - prompt->px (44), prompt->px (12), prompt->px (32), prompt->px (32), SWP_NOZORDER);
            ::InvalidateRect (hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_NCHITTEST:
        {
            POINT point {GET_X_LPARAM (lp), GET_Y_LPARAM (lp)};
            ::ScreenToClient (hwnd, &point);
            RECT r {};
            ::GetClientRect (hwnd, &r);
            return point.y < prompt->px (54) && point.x < r.right - prompt->px (48) ? HTCAPTION : HTCLIENT;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT:
        case WM_PRINTCLIENT:
        {
            PAINTSTRUCT ps {};
            HDC dc = msg == WM_PAINT ? ::BeginPaint (hwnd, &ps) : reinterpret_cast<HDC> (wp);
            RECT r {};
            ::GetClientRect (hwnd, &r);
            ::FillRect (dc, &r, prompt->background);
            HBRUSH border = ::CreateSolidBrush (color (theme.rule));
            ::FrameRect (dc, &r, border);
            RECT line {prompt->px (24), prompt->px (53), r.right - prompt->px (24), prompt->px (54)};
            ::FillRect (dc, &line, border);
            RECT input {prompt->px (24), prompt->px (94), r.right - prompt->px (24), prompt->px (134)};
            ::FillRect (dc, &input, prompt->surface);
            const bool focused = ::GetFocus () == prompt->edit;
            HBRUSH inputBorder = ::CreateSolidBrush (color (focused ? theme.accent : theme.rule));
            ::FrameRect (dc, &input, inputBorder);
            ::DeleteObject (inputBorder);
            ::DeleteObject (border);
            ::SetBkMode (dc, TRANSPARENT);
            ::SetTextColor (dc, color (theme.ink));
            const auto old = ::SelectObject (dc, prompt->titleFont);
            RECT title {prompt->px (24), prompt->px (12), r.right - prompt->px (52), prompt->px (44)};
            ::DrawTextW (dc, L"打开网络视频", -1, &title, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            ::SelectObject (dc, old);
            if (msg == WM_PAINT) ::EndPaint (hwnd, &ps);
            return 0;
        }
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLOREDIT:
        {
            HDC dc = reinterpret_cast<HDC> (wp);
            ::SetTextColor (dc, color (theme.ink));
            ::SetBkColor (dc, color (msg == WM_CTLCOLOREDIT ? theme.surface : theme.paper));
            return reinterpret_cast<LRESULT> (msg == WM_CTLCOLOREDIT ? prompt->surface : prompt->background);
        }
        case WM_DRAWITEM:
        {
            auto* item = reinterpret_cast<DRAWITEMSTRUCT*> (lp);
            const bool close = item->CtlID == 4;
            const bool pressed = (item->itemState & ODS_SELECTED) != 0;
            HBRUSH fill = ::CreateSolidBrush (color (pressed ? theme.accentSoft : close ? theme.paper : theme.surface));
            ::FillRect (item->hDC, &item->rcItem, fill);
            ::DeleteObject (fill);
            if (!close)
            {
                HBRUSH edge = ::CreateSolidBrush (color (item->CtlID == 2 && !(item->itemState & ODS_DISABLED) ? theme.accent : theme.rule));
                ::FrameRect (item->hDC, &item->rcItem, edge);
                ::DeleteObject (edge);
            }
            ::SetBkMode (item->hDC, TRANSPARENT);
            ::SetTextColor (item->hDC, color (item->itemState & ODS_DISABLED ? theme.muted : theme.ink));
            const auto old = ::SelectObject (item->hDC, close ? prompt->iconFont : prompt->font);
            ::DrawTextW (item->hDC, close ? L"\xE8BB" : item->CtlID == 2 ? L"打开" : L"取消", -1,
                &item->rcItem, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            ::SelectObject (item->hDC, old);
            if (item->itemState & ODS_FOCUS)
            {
                RECT focus = item->rcItem;
                ::InflateRect (&focus, -prompt->px (3), -prompt->px (3));
                ::DrawFocusRect (item->hDC, &focus);
            }
            return TRUE;
        }
        case WM_COMMAND:
            if (LOWORD (wp) == 1)
            {
                if (HIWORD (wp) == EN_CHANGE)
                {
                    wchar_t text[2048] {};
                    ::GetWindowTextW (prompt->edit, text, 2048);
                    ::EnableWindow (prompt->open, std::wstring (text).find_first_not_of (L" \t\r\n") != std::wstring::npos);
                }
                ::InvalidateRect (hwnd, nullptr, FALSE);
                return 0;
            }
            if (LOWORD (wp) == 2)
            {
                wchar_t text[2048] = {};
                ::GetWindowTextW (prompt->edit, text, 2048);
                const std::wstring url (text);
                const auto first = url.find_first_not_of (L" \t\r\n");
                if (first == std::wstring::npos) return 0;
                *prompt->value = url.substr (first, url.find_last_not_of (L" \t\r\n") - first + 1);
                prompt->accepted = true;
                ::DestroyWindow (hwnd);
                return 0;
            }
            if (LOWORD (wp) == 3 || LOWORD (wp) == 4)
            {
                ::DestroyWindow (hwnd);
                return 0;
            }
            break;
        case WM_CLOSE:
            ::DestroyWindow (hwnd);
            return 0;
    }
    return ::DefWindowProcW (hwnd, msg, wp, lp);
}

bool promptVideoUrl (HWND owner, std::wstring& value, const ap::ui::Theme& theme,
                     double scale, HFONT font, HFONT titleFont, HFONT iconFont)
{
    WNDCLASSEXW wc {};
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = videoUrlPromptProc;
    wc.hInstance = ::GetModuleHandleW (nullptr);
    wc.hCursor = ::LoadCursorW (nullptr, IDC_ARROW);
    wc.lpszClassName = kVideoUrlPromptClass;
    ::RegisterClassExW (&wc);

    RECT ownerRect {};
    ::GetWindowRect (owner, &ownerRect);
    const DWORD style = WS_POPUP | WS_CLIPCHILDREN;
    const DWORD exStyle = WS_EX_DLGMODALFRAME;
    VideoUrlPrompt prompt;
    prompt.theme = theme;
    prompt.scale = scale;
    prompt.font = font;
    prompt.titleFont = titleFont;
    prompt.iconFont = iconFont;
    const auto color = [] (unsigned rgb) { return RGB ((rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255); };
    prompt.background = ::CreateSolidBrush (color (theme.paper));
    prompt.surface = ::CreateSolidBrush (color (theme.surface));
    const int width = prompt.px (520);
    const int height = prompt.px (220);
    const int ownerWidth = static_cast<int> (ownerRect.right - ownerRect.left);
    const int ownerHeight = static_cast<int> (ownerRect.bottom - ownerRect.top);
    const int x = static_cast<int> (ownerRect.left) + std::max (0, (ownerWidth - width) / 2);
    const int y = static_cast<int> (ownerRect.top) + std::max (0, (ownerHeight - height) / 2);
    prompt.value = &value;
    prompt.window = ::CreateWindowExW (exStyle, kVideoUrlPromptClass,
        L"打开网络视频", style,
        x, y, width, height,
        owner, nullptr, wc.hInstance, &prompt);
    if (!prompt.window)
    {
        ::DeleteObject (prompt.background);
        ::DeleteObject (prompt.surface);
        return false;
    }

    ::EnableWindow (owner, FALSE);
    ::ShowWindow (prompt.window, SW_SHOW);
    ::UpdateWindow (prompt.window);
    ::SetFocus (prompt.edit);
    MSG msg {};
    while (::IsWindow (prompt.window) && ::GetMessageW (&msg, nullptr, 0, 0) > 0)
    {
        if (msg.message == WM_KEYDOWN && (msg.hwnd == prompt.window || ::IsChild (prompt.window, msg.hwnd)))
        {
            if (msg.wParam == VK_ESCAPE || msg.wParam == VK_RETURN)
            {
                const int command = msg.wParam == VK_ESCAPE ? 3 :
                    msg.hwnd == prompt.cancel ? 3 : msg.hwnd == prompt.close ? 4 : 2;
                ::SendMessageW (prompt.window, WM_COMMAND, command, 0);
                continue;
            }
        }
        if (!::IsDialogMessageW (prompt.window, &msg))
        {
            ::TranslateMessage (&msg);
            ::DispatchMessageW (&msg);
        }
    }
    ::EnableWindow (owner, TRUE);
    ::SetForegroundWindow (owner);
    ::SetFocus (owner);
    ::DeleteObject (prompt.background);
    ::DeleteObject (prompt.surface);
    if (msg.message == WM_QUIT) ::PostQuitMessage (static_cast<int> (msg.wParam));
    return prompt.accepted;
}

// 拍号选项（与 macOS 端一致）
struct TimeSig { const wchar_t* label; int beats; int denom; };
const TimeSig kTimeSigs[] = {
    { L"4/4",  4, 4 }, { L"3/4",  3, 4 }, { L"2/4",  2, 4 }, { L"5/4",  5, 4 },
    { L"6/8",  6, 8 }, { L"7/8",  7, 8 }, { L"9/8",  9, 8 }, { L"12/8", 12, 8 },
};
const int kTimeSigCount = static_cast<int> (sizeof (kTimeSigs) / sizeof (kTimeSigs[0]));

// 拍号设置弹层里的按钮：kind 0 = 分子选项，1 = 分母选项，2 = 取消该小节的变化
struct TsPopupBtn { RECT rc; int kind; int value; };

// 拍号条上的方块（波形区坐标）
struct TsStripBox { RECT rc; int bar = 1; int beats = 4; int denom = 4; bool changed = false; };

// 候选拍号（与 macOS 端逐值一致）：分子取常用值，分母只留 2/4/8/16
const int kTsBeatChoices[]  = { 2, 3, 4, 5, 6, 7, 9, 12 };
const int kTsDenomChoices[] = { 2, 4, 8, 16 };
const int kTsBeatChoiceCount  = static_cast<int> (sizeof (kTsBeatChoices)  / sizeof (kTsBeatChoices[0]));
const int kTsDenomChoiceCount = static_cast<int> (sizeof (kTsDenomChoices) / sizeof (kTsDenomChoices[0]));

//------------------------------------------------------------------------------
std::wstring utf8ToWide (const std::string& s)
{
    if (s.empty ())
        return std::wstring ();
    const int n = ::MultiByteToWideChar (CP_UTF8, 0, s.c_str (),
                                         static_cast<int> (s.size ()), nullptr, 0);
    if (n <= 0)
        return std::wstring ();
    std::wstring w (static_cast<size_t> (n), L'\0');
    ::MultiByteToWideChar (CP_UTF8, 0, s.c_str (), static_cast<int> (s.size ()), &w[0], n);
    return w;
}

std::string wideToUtf8 (const std::wstring& w)
{
    if (w.empty ())
        return std::string ();
    const int n = ::WideCharToMultiByte (CP_UTF8, 0, w.c_str (),
                                         static_cast<int> (w.size ()), nullptr, 0, nullptr, nullptr);
    if (n <= 0)
        return std::string ();
    std::string s (static_cast<size_t> (n), '\0');
    ::WideCharToMultiByte (CP_UTF8, 0, w.c_str (), static_cast<int> (w.size ()),
                           &s[0], n, nullptr, nullptr);
    return s;
}

// 统一：用 UTF-8 字符串设置控件文本，避免宽/窄格式化串混用
void setTextIfChanged (HWND h, const std::wstring& text)
{
    if (!h)
        return;
    wchar_t current[128] = {};
    ::GetWindowTextW (h, current, 128);
    if (text != current)
        ::SetWindowTextW (h, text.c_str ());
}

std::string formatTime (double sec)
{
    if (sec < 0.0)
        sec = 0.0;
    const int total = static_cast<int> (sec + 0.5);
    char buf[32];
    std::snprintf (buf, sizeof buf, "%d:%02d", total / 60, total % 60);
    return std::string (buf);
}

int modsNow ()
{
    int m = 0;
    if (::GetKeyState (VK_CONTROL) < 0) m |= MK_CONTROL;
    if (::GetKeyState (VK_MENU)    < 0) m |= AP_MK_ALT;
    if (::GetKeyState (VK_SHIFT)   < 0) m |= MK_SHIFT;
    return m;
}

//------------------------------------------------------------------------------
// 高 DPI 缩放
//
// 背景：宿主进程若是「每显示器 DPI 感知」（Per-Monitor Aware），我们拿到的窗口
// 坐标就是**物理像素**。写死的 340×384 布局在 150% / 200% 缩放的屏幕上会被画成
// 物理尺寸极小的面板 —— 这就是「界面太小」的根因。
//
// 对策：按窗口实际 DPI 把整套布局（面板尺寸、控件坐标、字号、波形区）等比放大。
//
// 宿主若是 DPI 无感知（Windows 会整窗位图拉伸放大），GetDpiForWindow 返回 96
// → 缩放系数 1.0，行为与改动前完全一致。所以这条改动对老环境是**无副作用**的。
//
// 三个 API 依次降级，保证 Win7 也能跑：
//   GetDpiForWindow (Win10 1607+) → GetDpiForMonitor (Win8.1+) → GetDeviceCaps
// 全部用 GetProcAddress 动态取，不引入新的链接期依赖。
//------------------------------------------------------------------------------
double g_uiScale = 1.0;   ///< 模块级缩放系数（getSize 在 attached 之前被调用时用它）

using GetDpiForWindowFn  = UINT    (WINAPI*) (HWND);
using GetDpiForMonitorFn = HRESULT (WINAPI*) (HMONITOR, int, UINT*, UINT*);

double rawDpiScaleFor (HWND hwnd)
{
    if (!hwnd)
        hwnd = ::GetForegroundWindow ();

    if (HMODULE user32 = ::GetModuleHandleW (L"user32.dll"))
    {
        auto p = reinterpret_cast<GetDpiForWindowFn> (
            reinterpret_cast<void*> (::GetProcAddress (user32, "GetDpiForWindow")));
        if (p && hwnd)
        {
            const UINT dpi = p (hwnd);
            if (dpi >= 96)
                return static_cast<double> (dpi) / 96.0;
        }
    }

    if (HMODULE shcore = ::LoadLibraryW (L"shcore.dll"))
    {
        auto p = reinterpret_cast<GetDpiForMonitorFn> (
            reinterpret_cast<void*> (::GetProcAddress (shcore, "GetDpiForMonitor")));
        UINT dx = 0, dy = 0;
        if (p && SUCCEEDED (p (::MonitorFromWindow (hwnd, MONITOR_DEFAULTTOPRIMARY),
                               0 /*MDT_EFFECTIVE_DPI*/, &dx, &dy)) && dx >= 96)
        {
            ::FreeLibrary (shcore);
            return static_cast<double> (dx) / 96.0;
        }
        ::FreeLibrary (shcore);
    }

    if (HDC dc = ::GetDC (nullptr))
    {
        const int dpi = ::GetDeviceCaps (dc, LOGPIXELSX);
        ::ReleaseDC (nullptr, dc);
        if (dpi >= 96)
            return static_cast<double> (dpi) / 96.0;
    }
    return 1.0;
}

double uiScaleFor (HWND hwnd)
{
    double s = rawDpiScaleFor (hwnd);
    // 夹到 [1.0, 3.0]，再量化到 0.25 的档位：避免 137/96 这类零碎比例
    // 让控件落在半像素上、文字发虚、边框粗细不一。
    if (s < 1.0) s = 1.0;
    if (s > 3.0) s = 3.0;
    s = std::round (s * 4.0) / 4.0;
    return s < 1.0 ? 1.0 : s;
}

int scaledOf (int designPx) { return static_cast<int> (std::lround (designPx * g_uiScale)); }
int preferredPanelW () { return scaledOf (kPanelW); }
int preferredPanelH () { return scaledOf (kPanelH); }
int minPanelW () { return scaledOf (kPanelWMin); }
int minPanelH () { return scaledOf (kPanelHMin); }
int maxPanelW () { return scaledOf (kPanelWMax); }
int maxPanelH () { return scaledOf (kPanelHMax); }

/// 把宿主/用户给的窗口尺寸钳到 [min, max]。
void clampPanelSize (int& w, int& h)
{
    if (w < minPanelW ()) w = minPanelW ();
    if (h < minPanelH ()) h = minPanelH ();
    if (w > maxPanelW ()) w = maxPanelW ();
    if (h > maxPanelH ()) h = maxPanelH ();
}

} // namespace

namespace ap {

//------------------------------------------------------------------------------
// 内部窗口对象
//------------------------------------------------------------------------------
class WinView
{
public:
    explicit WinView (PlugView::Backend* backend) : m_backend (backend) {}

    bool create (HWND parent, int w, int h);
    void destroy ();
    void resize (int w, int h);
    void paint (HDC dc, const RECT& rc);
    void paintChrome (HDC dc);
    bool drawItem (const DRAWITEMSTRUCT* dis);
    /// 使用指南自绘（由【主窗口】的 WM_PAINT 调用，见 setHelp 的说明）。
    /// dc 的原点即主窗口客户区左上角。
    void paintHelp (HDC dc);
    /// 显示/隐藏使用指南。
    void toggleHelp ();
    /// 关闭使用指南（主窗口被点到时回调）。
    void hideHelp ();
    void refreshUpdates ();
    void updateAction ();
    bool helpVisible () const { return m_helpVisible; }
    void setCollapsed (bool collapsed);
    void paintFloating (HDC dc);
    void layoutFloating ();
    void moveFloatingVideo ();
    void setVideoOpacity ();
    LRESULT floatingHitTest (POINT screen) const;

    // ---- 拍号设置弹层（点波形顶部拍号条上的方块后弹出）----
    /// 弹出第 bar 小节的拍号设置面板。
    void openTimeSigPopup (int bar);
    /// 关闭拍号设置面板并恢复控件显示。
    void closeTimeSigPopup ();
    /// 把「第 bar 小节起换成 beats/denom」写进后端（等于继承值时自动改为取消变化）。
    void applyTimeSigChange (int bar, int beats, int denom);
    /// 取消第 bar 小节处的拍号变化。
    void removeTimeSigChange (int bar);
    /// 弹层是否可见。
    bool tsPopupVisible () const { return m_tsPopupVisible; }
    /// 自绘弹层（由【主窗口】的 WM_PAINT 调用；dc 原点 = 主窗口客户区左上角）。
    void paintTimeSigPopup (HDC dc);
    /// 弹层上的鼠标按下 → 返回 true 表示已处理（含「点在面板外 = 关闭」）。
    bool onTimeSigPopupClick (int x, int y);
    /// 弹层里各按钮的矩形（绘制与命中测试共用同一份）。
    void layoutTsPopup ();
    /// 波形区坐标 (x,y) 是否落在拍号条某个方块上；命中则回填小节号。
    bool waveTimeSigBoxAt (int x, int y, int& bar) const;
    /// 生成当前视野的小节表（拍号变化逐小节推进；算法三端共用于 gui.h）。
    void rebuildBars (std::vector<ap::BarMark>& out) const;
    /// 当前视野下拍号条的方块布局（绘制与点击命中共用同一份）。
    std::vector<TsStripBox> tsStripBoxes () const;
    /// 画拍号条（画在波形区的离屏 DC 上，dc 原点 = 波形区左上角）。
    void drawTimeSigStrip (HDC dc, const std::vector<TsStripBox>& boxes);
    void onMouseDown (int x, int y, int mods);
    void onMouseMove (int x, int y, int mods);
    void onMouseUp (int x, int y, bool cancel = false);
    /// 滚轮。horizontal = 横向滚轮（WM_MOUSEHWHEEL / 触控板左右滑）。
    void onWheelDelta (int delta, int mods, bool horizontal);
    /// 光标是否落在波形区上（形参是【屏幕】坐标，滚轮的 lParam 即屏幕坐标）。
    bool waveHitTest (int screenX, int screenY) const;
    void onDoubleClick ();
    void onTimer ();
    void onDropFiles (HDROP drop);
    void onCommand (int id, int notify);
    void onHScroll (HWND slider);
    bool onNumericWheel (HWND control, int delta, bool fine, bool coarse = false);
    void paintSlider (HWND slider, HDC dc);
    void paintTimeSigControl (HDC dc);
    void setVolumeAt (int x);
    void setProgressAt (int x);
    void finishProgress (bool cancel);
    bool onProgressKey (WPARAM key);
    void onTempoClick (int x, int y);
    LRESULT onNotify (NMHDR* notification);
    void hoverControl (HWND control)
    {
        if (m_hotControl == control) return;
        if (m_hotControl) ::InvalidateRect (m_hotControl, nullptr, FALSE);
        m_hotControl = control;
        if (m_hotControl) ::InvalidateRect (m_hotControl, nullptr, FALSE);
    }
    HBRUSH onCtlColor (HDC dc, HWND ctl);

    /// DPI 变了（跨屏拖动、系统缩放改动）时重算缩放系数并重建字体/布局。
    void onDpiChanged ();
    /// 绑定宿主侧回调，供 onDpiChanged 请求改窗口尺寸。
    void setOwner (PlugView* o) { m_owner = o; }

    HWND hwnd () const { return m_hwnd; }
    /// 当前窗口尺寸（物理像素）—— getSize 要如实汇报，否则宿主的缩放框架会打架。
    int panelW () const { return m_panelW; }
    int panelH () const { return m_panelH; }

private:
    void layoutChildren ();
    /// 真正应用新的客户区尺寸（updateDesign = 是否把这次尺寸记为用户选择）。
    void applyPanelSize (int w, int h, bool updateDesign);
    /// 宿主容器装不下时缩回去，防止右侧控件被父窗口裁掉。
    void syncSizeToParent ();
    void createFonts ();
    void deleteFonts ();
    void applyFontsToChildren ();
    void refreshLabels ();
    void seekFromAudio (double audioSec);
    void openFileDialog ();
    void openVideoFileDialog ();
    void openVideoUrlDialog ();
    void loadAudio (const std::wstring& path);
    void loadVideo (const std::wstring& target, bool local);
    void closeMedia ();
    void presentMedia ();
    double mediaDuration () const;
    double mediaPosition () const;
    void syncCurrentScore (bool showStatus);
    void applyNumericEdit (int id);
    void showThemeMenu ();
    void applyTheme (int theme);
    void applyBpmFromEdit ();
    void applyPitchFromEdit (int id);
    /// ⭐ 视图重建后从后端回读 BPM / 拍号 / 音量（见实现处的说明）
    void syncControlsFromBackend ();
    /// 显示/隐藏使用指南（隐藏期间所有控件一并隐藏，靠主窗口整块自绘）。
    void setHelp (bool on);
    /// 批量显示/隐藏所有控件窗口（只有使用指南要这么做）。
    void showControls (bool show);
    /// 记下「用户正在手动看别处」→ 2 秒内不自动跟随。
    void markUserScrolling ();
    /// 用户停手超时后自动恢复跟随。
    void recoverAutoFollow ();
    /// 视野跟随播放头（规则见 kHostJumpSec 处的说明）。
    void followPlayheadTo (double audioSec, bool allowJump);
    /// 请求重绘波形区。fromMouse=true 时按 kDragPaintGapMs 节流（鼠标消息太密）。
    void requestWaveRepaint (bool fromMouse);
    void zoomBy (double factor, double centerSec);
    void zoomFit ();
    /// 把视野挪到 sec（播放头落在画面 25% 处）。用于「回到播放头」。
    void centerViewOn (double sec);
    void clampView ();
    void syncVideoToTimeline (bool force);
    void afterLoad ();
    /// ⭐ 视图重建后把后端里存的视野读回来（见实现处的说明）
    void restoreViewState ();
    /// 新载入音频的默认视野（afterLoad 与 restoreViewState 共用，避免两份规则漂移）
    void applyDefaultView ();
    double xToSec (int x) const;
    int    secToX (double sec) const;

    PlugView::Backend* m_backend = nullptr;

    HWND m_hwnd = nullptr;
    HWND m_wave = nullptr;
    HWND m_floating = nullptr, m_expandBtn = nullptr, m_collapseBtn = nullptr;
    HWND m_opacitySlider = nullptr;
    int m_videoOpacity = 100;
    HWND m_editorShell = nullptr;
    bool m_collapsed = false, m_shellWasVisible = false;
    std::wstring m_floatingText;
    std::wstring m_scoreTempoText;
    // ⚠️ 使用指南【不是】独立窗口，而是主窗口自己画的一层（见 setHelp）。
    //    旧版把它做成最后一个创建的子窗口，指望「创建顺序 = z 序最上」；
    //    用户实测在 MuseScore 里没生效：帮助画在了主界面【下面】，
    //    「？帮助」看起来像没反应。改成「隐藏所有控件 + 父窗口整块自绘」后，
    //    层级问题从根上不存在（Linux 端一直是这么做的）。
    bool m_helpVisible = false;
    updates::Client m_updater {MUSETONE_VERSION};
    updates::Status m_updateStatus;
    HWND m_updateCheck = nullptr, m_updateAction = nullptr;
    bool m_autoUpdateChecked = false;

    // ---- 拍号设置弹层 ----
    // ⚠️ 与使用指南同样的理由：不做独立子窗口，而是「隐藏所有控件 + 主窗口整块自绘」
    //    —— 宿主里的 z 序不可依赖（铁律 15），层级不参与就不会错。
    bool m_tsPopupVisible   = false;
    int  m_tsPopupBar       = 1;
    int  m_tsPopupBeats     = 4;
    int  m_tsPopupDenom     = 4;
    bool m_tsPopupHasChange = false;
    std::vector<TsPopupBtn> m_tsPopupBtns;   ///< 按钮矩形（绘制与命中测试共用同一份）
    RECT m_tsPopupBox = { 0, 0, 0, 0 };      ///< 弹层面板矩形
    HWND m_openBtn = nullptr, m_videoBtn = nullptr, m_closeMediaBtn = nullptr, m_zoomOut = nullptr, m_zoomIn = nullptr, m_zoomFit = nullptr;
    std::vector<HWND> m_spaceSyncControls;
    HWND m_backBtn = nullptr, m_helpBtn = nullptr, m_gridCheck = nullptr, m_numCheck = nullptr;
    HWND m_bpmEdit = nullptr, m_bpmUp = nullptr, m_bpmDown = nullptr;
    HWND m_timesig = nullptr, m_volSlider = nullptr, m_offsetZero = nullptr;
    HWND m_progressSlider = nullptr;
    bool m_progressDragging = false;
    HWND m_offsetEdit = nullptr, m_offsetDown = nullptr, m_offsetUp = nullptr;
    HWND m_wheelControl = nullptr;
    int m_numericWheelRemainder = 0;
    bool m_syncingControls = false;
    HWND m_semitoneEdit = nullptr, m_semitoneUp = nullptr, m_semitoneDown = nullptr;
    HWND m_centsEdit = nullptr, m_centsUp = nullptr, m_centsDown = nullptr;
    HWND m_pitchReset = nullptr;
    HWND m_scoreBtn = nullptr, m_themeBtn = nullptr, m_followScoreCheck = nullptr;
    HWND m_tooltip = nullptr;
    HWND m_hotControl = nullptr;
    ui::Layout m_layout = ui::arrange (kPanelW, kPanelH);
    std::wstring m_timeText, m_offsetText, m_sourceText, m_volumeText;
    std::wstring m_musicalText = L"--";
    std::wstring m_fileText, m_transportText, m_pitchText;
    std::wstring m_detectedTempoText;
    std::shared_ptr<const TempoResult> m_tempoResult;
    double m_tempoPosition = -1;
    std::string m_displayedPath;
    std::unique_ptr<VideoView> m_videoView;
    std::wstring m_videoTarget;
    bool m_destroying = false;

    HFONT m_font = nullptr, m_fontSmall = nullptr, m_fontBold = nullptr, m_fontIcons = nullptr;
    HFONT m_fontHeading = nullptr, m_fontMono = nullptr, m_fontPosition = nullptr;
    int   m_tsBoxW = 0;   ///< 拍号条方块宽（按 "12/16" 实测；createFonts 里算）
    HBRUSH m_bgBrush = nullptr;
    HBRUSH m_inputBrush = nullptr;
    int m_theme = -1;
    COLORREF kPaper, kSurface, kInk, kMuted, kRule, kSoftRule;
    COLORREF kAccent, kAccentDark, kAccentSoft, kOlive;
    std::wstring m_meterText;
    int m_followControls = -1;
    std::string m_scoreSnapshot;
    std::string m_scoreSession;
    DWORD m_lastScoreCheck = 0;

    PlugView* m_owner = nullptr;      ///< 宿主侧回调（DPI 变化时请求改窗口尺寸）

    double m_viewStart = 0.0;   ///< 视野起点（秒）
    double m_viewSpan = 8.0;    ///< 视野跨度（秒）

    //---- 视野状态回存（关掉编辑器再打开要恢复到同一段视野）----
    // 视图窗口每次打开都是新建的，视野得存在后端（Processor）里；onTimer 里发现
    // 视野变了就写回。改动入口很多（滚轮/拖动/缩放/全览/回到播放头/自动跟随），
    // 集中在一处比对才不会漏（见 MEMORY 铁律 9）。-1 = 还没写过。
    double m_pushedViewStart = -1.0;
    double m_pushedViewSpan  = -1.0;

    //---- 视野自动跟随（规则与 macOS 端一致）----
    bool   m_followScroll = true;    ///< 总开关（现阶段恒为 true）
    bool   m_userScrolling = false;  ///< 用户手动操作期间暂停跟随
    DWORD  m_lastUserScrollTick = 0; ///< 用户最后一次手动操作的时刻（做超时恢复）
    bool   m_hasPrevPlayhead = false; ///< 是否已有上一帧谱面位置（判断宿主跳变）
    double m_prevPlayheadSec = 0.0;

    //---- 「点完『回到播放头』仍错位」检测（onTimer 维护，paint 只读）----
    DWORD m_lastBackTick = 0;          ///< 上次点「回到播放头」的时刻（毫秒）
    bool  m_followFail = false;        ///< 是否处于「点了按钮仍错位」状态
    DWORD m_followFailLoggedTick = 0;  ///< 上次为此写日志的时刻（做冷却）

    //---- 视频跟随 MuseScore 谱面时间轴 ----
    DWORD m_lastVideoSyncTick = 0;
    double m_lastVideoSyncSec = -1.0;
    double m_lastVideoOffsetSec = 0.0;
    bool m_lastVideoPlaying = false;
    bool m_videoSyncInitialized = false;
    VideoTimeline m_videoTimeline;
    double m_videoScoreSec = 0.0;

    //---- 高 DPI ----
    double m_scale = 1.0;             ///< DPI 缩放系数（1.0 = 96 DPI）
    int    m_panelW = kPanelW;        ///< 当前窗口宽（物理像素）
    int    m_panelH = kPanelH;        ///< 当前窗口高（物理像素）
    int    m_designW = kPanelW;       ///< 用户选择的逻辑宽（设计像素，跨 DPI 时按它换算）
    int    m_designH = kPanelH;
    int    m_waveW = kWaveW;          ///< 缩放后的波形区宽（secToX 的基准）
    int    m_waveH = kWaveH;          ///< 缩放后的波形区高

    /// 离屏位图复用。旧版每次 paint 都 CreateCompatibleBitmap/DC ——
    /// 拖动时每帧一次 GDI 分配，在虚拟机上足够把音频线程饿出爆音。
    HDC     m_memDC = nullptr;
    HBITMAP m_memBmp = nullptr;
    int     m_memW = 0, m_memH = 0;

    /// 上一次真正画过的内容指纹（位置/视野）。变了才重绘，避免空转 20Hz 重画。
    double m_lastPaintKey = 1.0e30;

    /// 把「96 DPI 下的设计像素」换算成当前 DPI 下的物理像素。
    int px (int designPx) const
    {
        return static_cast<int> (std::lround (static_cast<double> (designPx) * m_scale));
    }

    bool m_dragging = false;      ///< 波形区按住鼠标中
    bool m_dragOffset = false;    ///< true = 改起始偏移；false = 平移视野
    int  m_dragStartX = 0;
    int m_dragStartY = 0;
    bool m_dragMoved = false;
    double m_dragStartVal = 0.0;  ///< 拖动开始时被改的量（改偏移时 = offset，平移时 = viewStart）
    DWORD m_lastMousePaintTick = 0;   ///< 鼠标驱动的上次重绘时刻（节流用）
    bool  m_mousePaintPending = false; ///< 有被节流掉的重绘，等定时器补

    bool m_showGrid = true;
    bool m_showNumbers = true;
};

/// 子控件拿到键盘焦点时，空格消息会先到达子控件。统一子类化这些控件，
/// 让插件窗口内任意焦点位置都能同步宿主播放/暂停。
static LRESULT CALLBACK spaceSyncSubclassProc (HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                                               UINT_PTR id, DWORD_PTR ref);
bool forwardSpaceToHost (WinView* v, LPARAM lp);

/// SetWindowSubclass 的子类 ID（同一控件可挂多个子类，用 ID 区分；必须唯一且非 0）
static const UINT_PTR kSpaceSyncSubclassId = 2;

//------------------------------------------------------------------------------
// 窗口过程与类注册
//------------------------------------------------------------------------------
namespace {

LRESULT CALLBACK waveProc (HWND, UINT, WPARAM, LPARAM);
LRESULT CALLBACK mainProc (HWND, UINT, WPARAM, LPARAM);
LRESULT CALLBACK floatingProc (HWND, UINT, WPARAM, LPARAM);

WinView* viewOf (HWND hwnd)
{
    return reinterpret_cast<WinView*> (::GetWindowLongPtrW (hwnd, GWLP_USERDATA));
}

void ensureClasses ()
{
    static bool done = false;
    if (done)
        return;
    done = true;

    HINSTANCE hinst = ::GetModuleHandleW (nullptr);

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof wc;
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = mainProc;
    wc.hInstance = hinst;
    wc.hCursor = ::LoadCursorW (nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = kMainClass;
    ::RegisterClassExW (&wc);

    WNDCLASSEXW wv = {};
    wv.cbSize = sizeof wv;
    wv.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wv.lpfnWndProc = waveProc;
    wv.hInstance = hinst;
    wv.hCursor = ::LoadCursorW (nullptr, IDC_ARROW);
    wv.hbrBackground = nullptr;
    wv.lpszClassName = kWaveClass;
    ::RegisterClassExW (&wv);

    wc.lpfnWndProc = floatingProc;
    wc.style = CS_DBLCLKS;
    wc.lpszClassName = kFloatingClass;
    ::RegisterClassExW (&wc);
}

} // namespace

//------------------------------------------------------------------------------
void WinView::createFonts ()
{
    // 字号同样按 DPI 放大（lfHeight 取负 = 按字符高度指定，不是行距）。
    // 下限 -8，避免极小缩放时算出 0 让系统退回默认大字。
    LOGFONTW lf = {};
    lf.lfHeight = -std::max (8L, std::lround (13.0 * m_scale));
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfQuality = CLEARTYPE_QUALITY;
    ::wcscpy_s (lf.lfFaceName, LF_FACESIZE, L"Microsoft YaHei UI");
    m_font = ::CreateFontIndirectW (&lf);

    lf.lfHeight = -std::max (8L, std::lround (12.0 * m_scale));
    m_fontSmall = ::CreateFontIndirectW (&lf);

    lf.lfHeight = -std::max (8L, std::lround (20.0 * m_scale));
    lf.lfWeight = FW_SEMIBOLD;
    m_fontBold = ::CreateFontIndirectW (&lf);

    lf.lfHeight = -std::max (8L, std::lround (13.0 * m_scale));
    m_fontHeading = ::CreateFontIndirectW (&lf);
    lf.lfWeight = FW_NORMAL;
    ::wcscpy_s (lf.lfFaceName, LF_FACESIZE, L"Consolas");
    m_fontMono = ::CreateFontIndirectW (&lf);
    lf.lfHeight = -std::max (8L, std::lround (17.0 * m_scale));
    lf.lfWeight = FW_SEMIBOLD;
    m_fontPosition = ::CreateFontIndirectW (&lf);

    lf.lfHeight = -std::max (8L, std::lround (15.0 * m_scale));
    lf.lfWeight = FW_NORMAL;
    ::wcscpy_s (lf.lfFaceName, LF_FACESIZE, L"Segoe MDL2 Assets");
    m_fontIcons = ::CreateFontIndirectW (&lf);

    if (!m_font)      m_font = reinterpret_cast<HFONT> (::GetStockObject (DEFAULT_GUI_FONT));
    if (!m_fontSmall) m_fontSmall = m_font;
    if (!m_fontBold)  m_fontBold = m_font;
    if (!m_fontIcons) m_fontIcons = m_font;
    if (!m_fontHeading) m_fontHeading = m_font;
    if (!m_fontMono) m_fontMono = m_font;
    if (!m_fontPosition) m_fontPosition = m_font;

    // 拍号条方块的宽度：按最宽的拍号文字（"12/16"）【实测】量出来。
    // 绘制与点击命中必须用同一个宽度，写死一个数字迟早会对不上（字体/DPI 都会变）。
    m_tsBoxW = 0;
    if (HDC dc = ::CreateCompatibleDC (nullptr))
    {
        HGDIOBJ old = ::SelectObject (dc, m_fontSmall);
        SIZE sz = {};
        if (::GetTextExtentPoint32W (dc, L"12/16", 5, &sz))
            m_tsBoxW = std::max (px (26), static_cast<int> (sz.cx) + px (10));
        ::SelectObject (dc, old);
        ::DeleteDC (dc);
    }
    if (m_tsBoxW <= 0)
        m_tsBoxW = px (34);
}

void WinView::deleteFonts ()
{
    if (m_font && m_font != ::GetStockObject (DEFAULT_GUI_FONT))
        ::DeleteObject (m_font);
    if (m_fontSmall && m_fontSmall != m_font)
        ::DeleteObject (m_fontSmall);
    if (m_fontBold && m_fontBold != m_font)
        ::DeleteObject (m_fontBold);
    if (m_fontIcons && m_fontIcons != m_font)
        ::DeleteObject (m_fontIcons);
    for (HFONT font : {m_fontHeading, m_fontMono, m_fontPosition})
        if (font && font != m_font)
            ::DeleteObject (font);
    m_font = m_fontSmall = m_fontBold = m_fontIcons = nullptr;
    m_fontHeading = m_fontMono = m_fontPosition = nullptr;
}

void WinView::applyFontsToChildren ()
{
    const HWND normal[] = { m_openBtn, m_videoBtn, m_closeMediaBtn, m_scoreBtn, m_themeBtn, m_followScoreCheck, m_collapseBtn, m_expandBtn,
                            m_helpBtn, m_updateCheck, m_updateAction, m_zoomOut, m_zoomIn, m_zoomFit, m_backBtn,
                            m_gridCheck, m_numCheck, m_bpmEdit, m_bpmUp, m_bpmDown,
                            m_timesig, m_volSlider, m_progressSlider, m_offsetZero, m_offsetEdit, m_offsetDown, m_offsetUp,
                            m_semitoneEdit, m_semitoneUp, m_semitoneDown,
                            m_centsEdit, m_centsUp, m_centsDown, m_pitchReset };
    for (HWND c : normal)
        if (c)
            ::SendMessageW (c, WM_SETFONT, reinterpret_cast<WPARAM> (m_font), TRUE);
    for (HWND c : {m_bpmEdit, m_offsetEdit, m_semitoneEdit, m_centsEdit})
        if (c)
            ::SendMessageW (c, WM_SETFONT, reinterpret_cast<WPARAM> (m_fontMono), TRUE);

    if (m_volSlider)
        ::SendMessageW (m_volSlider, TBM_SETTHUMBLENGTH, px (14), 0);
}

void WinView::onDpiChanged ()
{
    if (!m_hwnd)
        return;

    const double s = uiScaleFor (m_hwnd);
    if (std::fabs (s - m_scale) < 0.01)
        return;

    m_scale  = s;
    g_uiScale = s;

    // 逻辑尺寸（用户选的宽高）不变，物理尺寸按新 DPI 重算 ——
    // 跨屏拖动不该把用户拖好的窗口宽度改掉，只是整体放大。
    m_panelW = std::max (px (kPanelWMin), static_cast<int> (std::lround (m_designW * m_scale)));
    m_panelH = std::max (px (kPanelHMin), static_cast<int> (std::lround (m_designH * m_scale)));

    deleteFonts ();
    createFonts ();
    applyFontsToChildren ();
    layoutChildren ();

    if (m_memBmp) { ::DeleteObject (m_memBmp); m_memBmp = nullptr; m_memW = m_memH = 0; }

    ::SetWindowPos (m_hwnd, nullptr, 0, 0, m_panelW, m_panelH,
                    SWP_NOZORDER | SWP_NOMOVE);
    ::InvalidateRect (m_hwnd, nullptr, TRUE);
    if (m_wave)
        ::InvalidateRect (m_wave, nullptr, FALSE);

    if (m_floating && !m_videoView)
    {
        ::SetWindowPos (m_floating, nullptr, 0, 0, px (260), px (58), SWP_NOMOVE | SWP_NOZORDER);
        ::MoveWindow (m_expandBtn, px (220), px (11), px (32), px (36), TRUE);
        ::InvalidateRect (m_floating, nullptr, TRUE);
    }
    if (m_floating && m_videoView) layoutFloating ();

    // 面板尺寸变了，请宿主重新调整窗口（宿主不理也没关系，只是面板被裁一点）。
    if (m_owner)
        m_owner->requestResizeToPreferred ();
}

//------------------------------------------------------------------------------
bool WinView::create (HWND parent, int w, int h)
{
    ensureClasses ();

    // 先定缩放系数：后面所有尺寸（窗口、控件、字号、波形区）都按它放大。
    m_scale  = uiScaleFor (parent);
    g_uiScale = m_scale;

    clampPanelSize (w, h);
    m_panelW = w;
    m_panelH = h;
    m_designW = static_cast<int> (std::lround (m_panelW / m_scale));
    m_designH = static_cast<int> (std::lround (m_panelH / m_scale));

    INITCOMMONCONTROLSEX icc = {};
    icc.dwSize = sizeof icc;
    icc.dwICC = ICC_BAR_CLASSES | ICC_STANDARD_CLASSES | ICC_UPDOWN_CLASS;
    ::InitCommonControlsEx (&icc);

    applyTheme (m_backend ? m_backend->theme () : ui::defaultTheme);

    m_hwnd = ::CreateWindowExW (0, kMainClass, L"",
                                WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN,
                                0, 0, w, h, parent, nullptr,
                                ::GetModuleHandleW (nullptr), this);
    if (!m_hwnd)
        return false;

    createFonts ();

    HINSTANCE hi = ::GetModuleHandleW (nullptr);
    auto mk = [&] (const wchar_t* cls, const wchar_t* text, DWORD style, int id) -> HWND {
        const bool isButton = std::wcscmp (cls, L"BUTTON") == 0;
        if (isButton)
            style = (style & ~BS_TYPEMASK) | BS_OWNERDRAW;
        DWORD windowStyle = WS_CHILD | WS_VISIBLE | style;
        windowStyle |= WS_TABSTOP;
        HWND c = ::CreateWindowExW (0, cls, text, windowStyle,
                                    0, 0, 10, 10, m_hwnd,
                                    reinterpret_cast<HMENU> (static_cast<INT_PTR> (id)), hi, nullptr);
        if (c)
            ::SendMessageW (c, WM_SETFONT, reinterpret_cast<WPARAM> (m_font), TRUE);
        return c;
    };

    m_wave = mk (kWaveClass, L"", 0, 0);
    if (m_wave)
        ::SetWindowLongPtrW (m_wave, GWLP_USERDATA, reinterpret_cast<LONG_PTR> (this));

    m_openBtn   = mk (L"BUTTON", L"打开音频", BS_PUSHBUTTON, IDC_OPEN);
    m_videoBtn  = mk (L"BUTTON", L"打开视频", BS_PUSHBUTTON, IDC_OPEN_VIDEO);
    m_closeMediaBtn = mk (L"BUTTON", L"关闭素材", BS_PUSHBUTTON, IDC_CLOSE_MEDIA);
    m_scoreBtn = mk (L"BUTTON", L"当前谱面", BS_PUSHBUTTON, IDC_LINK_SCORE);
    m_themeBtn = mk (L"BUTTON", L"", BS_PUSHBUTTON, IDC_THEME_BTN);
    m_followScoreCheck = mk (L"BUTTON", L"跟随谱面", BS_PUSHBUTTON, IDC_FOLLOW_SCORE);
    // 「？帮助」：唤出使用指南覆盖层。有用户反馈不知道怎么用，说明书必须能在
    // 界面上直接点开，不能只躺在安装目录的 readme 里。
    m_helpBtn   = mk (L"BUTTON", L"?", BS_PUSHBUTTON, IDC_HELP_BTN);
    m_updateCheck = mk (L"BUTTON", L"检查更新", BS_PUSHBUTTON, IDC_UPDATE_CHECK);
    m_updateAction = mk (L"BUTTON", L"下载更新", BS_PUSHBUTTON, IDC_UPDATE_ACTION);
    ::ShowWindow (m_updateCheck, SW_HIDE);
    ::ShowWindow (m_updateAction, SW_HIDE);
    ::EnableWindow (m_updateAction, FALSE);
    m_collapseBtn = mk (L"BUTTON", L"", BS_PUSHBUTTON, IDC_COLLAPSE);
    m_zoomOut   = mk (L"BUTTON", L"−", BS_PUSHBUTTON, IDC_ZOOM_OUT);
    m_zoomIn    = mk (L"BUTTON", L"＋", BS_PUSHBUTTON, IDC_ZOOM_IN);
    m_zoomFit   = mk (L"BUTTON", L"全览", BS_PUSHBUTTON, IDC_ZOOM_FIT);
    // 原名「回到谱面」→「回到播放头」：用户想干的是「把我送回播放位置」，
    // 新名字直说结果，不需要先理解「谱面」和「音频」的关系。
    m_backBtn   = mk (L"BUTTON", L"回播放头", BS_PUSHBUTTON, IDC_BACK_TO_SCORE);
    m_gridCheck = mk (L"BUTTON", L"网格", BS_PUSHBUTTON, IDC_GRID_CHECK);
    m_numCheck  = mk (L"BUTTON", L"小节号", BS_PUSHBUTTON, IDC_NUM_CHECK);

    // 初值留空 = 自动；真实值在 createControls 末尾由 syncControlsFromBackend()
    // 从后端回读（关掉编辑器再打开时后端里还存着用户填的值）。
    m_bpmEdit = mk (L"EDIT", L"", ES_AUTOHSCROLL | ES_NUMBER | ES_CENTER, IDC_BPM_EDIT);
    ::SendMessageW (m_bpmEdit, EM_SETCUEBANNER, FALSE, reinterpret_cast<LPARAM> (L"--"));
    m_bpmUp   = mk (L"BUTTON", L"▲", BS_PUSHBUTTON, IDC_BPM_UP);
    m_bpmDown = mk (L"BUTTON", L"▼", BS_PUSHBUTTON, IDC_BPM_DOWN);

    m_timesig = mk (L"COMBOBOX", L"", CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED |
                   CBS_HASSTRINGS | CBS_NOINTEGRALHEIGHT | WS_VSCROLL, IDC_TIMESIG_COMBO);
    for (int i = 0; i < kTimeSigCount; ++i)
        ::SendMessageW (m_timesig, CB_ADDSTRING, 0,
                        reinterpret_cast<LPARAM> (kTimeSigs[i].label));
    ::SendMessageW (m_timesig, CB_SETCURSEL, 0, 0);

    m_volSlider = mk (TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS | TBS_FIXEDLENGTH,
                      IDC_VOLUME_SLIDER);
    if (m_volSlider)
    {
        ::SendMessageW (m_volSlider, TBM_SETRANGE, TRUE, MAKELPARAM (0, 100));
        ::SendMessageW (m_volSlider, TBM_SETPOS, TRUE, 100);
    }
    m_progressSlider = mk (TRACKBAR_CLASSW, L"播放进度", TBS_HORZ | TBS_NOTICKS | TBS_FIXEDLENGTH,
                           IDC_PROGRESS_SLIDER);
    ::SendMessageW (m_progressSlider, TBM_SETRANGE, TRUE, MAKELPARAM (0, 10000));
    m_offsetEdit = mk (L"EDIT", L"0", ES_AUTOHSCROLL | ES_CENTER, IDC_OFFSET_EDIT);
    m_offsetDown = mk (L"BUTTON", L"-", BS_PUSHBUTTON, IDC_OFFSET_DOWN);
    m_offsetUp = mk (L"BUTTON", L"+", BS_PUSHBUTTON, IDC_OFFSET_UP);

    // Whole-track pitch controls.  The edit boxes intentionally do not use
    // ES_NUMBER so negative values remain valid for downward shifts.
    m_semitoneEdit = mk (L"EDIT", L"0", ES_AUTOHSCROLL | ES_CENTER, IDC_SEMITONE_EDIT);
    m_semitoneUp = mk (L"BUTTON", L"▲", BS_PUSHBUTTON, IDC_SEMITONE_UP);
    m_semitoneDown = mk (L"BUTTON", L"▼", BS_PUSHBUTTON, IDC_SEMITONE_DOWN);
    m_centsEdit = mk (L"EDIT", L"0", ES_AUTOHSCROLL | ES_CENTER, IDC_CENTS_EDIT);
    m_centsUp = mk (L"BUTTON", L"▲", BS_PUSHBUTTON, IDC_CENTS_UP);
    m_centsDown = mk (L"BUTTON", L"▼", BS_PUSHBUTTON, IDC_CENTS_DOWN);
    m_pitchReset = mk (L"BUTTON", L"恢复原音", BS_PUSHBUTTON, IDC_PITCH_RESET);
    // 「归零」：一键把起始偏移清零。
    // 之前唯一的改偏移入口是「在波形上 Ctrl/Alt 拖动」，一旦手滑拖到很大的值
    // （或者只是想回到初始状态），就只能再反向拖回去 —— 拖回来的距离可能是好几个屏。
    // 用户实测明确提出「没有偏移归零的方式」，这里补上。
    m_offsetZero  = mk (L"BUTTON", L"归零", BS_PUSHBUTTON, IDC_OFFSET_ZERO);

    m_tooltip = ::CreateWindowExW (WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr,
        WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX, CW_USEDEFAULT, CW_USEDEFAULT,
        CW_USEDEFAULT, CW_USEDEFAULT, m_hwnd, nullptr, hi, nullptr);
    auto tip = [this] (HWND control, const wchar_t* text) {
        if (!m_tooltip || !control) return;
        TOOLINFOW tool = {};
        tool.cbSize = sizeof tool;
        tool.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
        tool.hwnd = m_hwnd;
        tool.uId = reinterpret_cast<UINT_PTR> (control);
        tool.lpszText = const_cast<wchar_t*> (text);
        ::SendMessageW (m_tooltip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM> (&tool));
    };
    tip (m_helpBtn, L"使用指南与更新");
    tip (m_videoBtn, L"打开本地视频或网络视频（支持 Bilibili 等网页）");
    tip (m_closeMediaBtn, L"关闭当前音频或视频");
    tip (m_updateCheck, L"检查 GitHub 正式发布的新版本");
    tip (m_updateAction, L"在插件内下载，校验后打开安装器；下载中可取消");
    tip (m_collapseBtn, L"收起为悬浮窗（继续同步）");
    tip (m_zoomOut, L"缩小波形");
    tip (m_zoomIn, L"放大波形");
    tip (m_zoomFit, L"整曲全览");
    tip (m_backBtn, L"回到播放头并重新对齐");
    tip (m_offsetZero, L"清零起始偏移");
    tip (m_offsetEdit, L"延迟（毫秒）；滚轮 10 ms，Ctrl + 滚轮 100 ms，Shift + 滚轮 1 ms");
    tip (m_offsetDown, L"偏移 -10 ms；Shift 为 -1 ms");
    tip (m_offsetUp, L"偏移 +10 ms；Shift 为 +1 ms");
    tip (m_volSlider, L"音量");
    tip (m_progressSlider, L"播放进度；点击或拖动跳转谱面");
    tip (m_pitchReset, L"恢复原始音高");
    tip (m_scoreBtn, L"刷新当前打开谱面的速度与拍号，不导入文件");
    tip (m_themeBtn, L"主题配色");
    tip (m_followScoreCheck, L"自动读取当前谱面；未连接时保留默认网格");
    tip (m_bpmEdit, L"关闭跟随谱面后可手动设置 BPM");
    tip (m_bpmUp, L"BPM +1");
    tip (m_bpmDown, L"BPM -1");
    tip (m_timesig, L"初始拍号");
    tip (m_semitoneEdit, L"半音：-24 至 +24");
    tip (m_semitoneUp, L"半音 +1");
    tip (m_semitoneDown, L"半音 -1");
    tip (m_centsEdit, L"音分：-100 至 +100");
    tip (m_centsUp, L"音分 +1");
    tip (m_centsDown, L"音分 -1");

    // 键盘焦点通常落在某个子控件上。给这些控件挂同一个轻量子类，保证
    // 编辑框、按钮、滑块和标签获得焦点时按空格仍能同步宿主播放/暂停。
    m_spaceSyncControls = {
        m_openBtn, m_videoBtn, m_closeMediaBtn, m_scoreBtn, m_themeBtn, m_followScoreCheck, m_collapseBtn,
        m_helpBtn, m_zoomOut, m_zoomIn, m_zoomFit, m_backBtn,
        m_gridCheck, m_numCheck, m_bpmEdit, m_bpmUp, m_bpmDown, m_timesig,
        m_volSlider, m_progressSlider, m_offsetZero, m_offsetEdit, m_offsetDown, m_offsetUp, m_semitoneEdit, m_semitoneUp,
        m_semitoneDown, m_centsEdit, m_centsUp, m_centsDown, m_pitchReset
    };
    for (HWND c : m_spaceSyncControls)
        if (c)
            ::SetWindowSubclass (c, spaceSyncSubclassProc, kSpaceSyncSubclassId,
                                 reinterpret_cast<DWORD_PTR> (this));

    // ⚠️ 使用指南不再是独立窗口（旧版这里创建了一个铺满面板的 kHelpClass 子窗口，
    //    用户实测它被画在主界面下面）。现在指南由主窗口自己整块画，见 setHelp。

    applyFontsToChildren ();

    // ⭐ 控件回读：关掉编辑器再打开时，窗口和所有控件都会重建，但后端（Processor）
    //    一直活着。不回读的话界面显示的是「初始值」，而后端里还存着用户填的值 ——
    //    BPM 输入框一失焦就会自动提交，等于把用户填的 BPM 覆盖成 120。
    syncControlsFromBackend ();

    // ⭐ 视野也要回读：关掉编辑器再打开时后端里还存着「上次放大到多少、
    //    正看着哪一段」，不回读就退回默认视野（用户实测反馈的就是这个）。
    restoreViewState ();
    refreshLabels ();

    ::DragAcceptFiles (m_hwnd, TRUE);
    if (m_wave)
        ::DragAcceptFiles (m_wave, TRUE);

    layoutChildren ();
    ::SetTimer (m_hwnd, IDC_TIMER_UI, 50, nullptr);   // 20Hz
    return true;
}

void WinView::destroy ()
{
    m_destroying = true;
    if (m_hwnd) ::KillTimer (m_hwnd, IDC_TIMER_UI);
    m_updater.stop ();
    if (m_videoView)
    {
        m_videoView->close ();
        m_videoView.reset ();
        if (m_backend) m_backend->setVideoAudio (nullptr);
    }
    // Restore only a shell that this view hid; never leave a hidden host dialog.
    if (m_collapsed && m_shellWasVisible && ::IsWindow (m_editorShell))
        ::ShowWindow (m_editorShell, SW_SHOWNOACTIVATE);
    m_collapsed = false;
    if (m_floating) ::DestroyWindow (m_floating);
    m_floating = m_expandBtn = m_opacitySlider = nullptr;
    if (m_hwnd)
    {
        ::KillTimer (m_hwnd, IDC_TIMER_UI);
        if (m_tooltip) ::DestroyWindow (m_tooltip);
        m_tooltip = nullptr;
        for (HWND c : m_spaceSyncControls)
            if (c)
                ::RemoveWindowSubclass (c, spaceSyncSubclassProc, kSpaceSyncSubclassId);
        m_spaceSyncControls.clear ();
        ::DestroyWindow (m_hwnd);    // 所有子控件随之销毁
        m_hwnd = nullptr;
    }
    m_wave = nullptr;
    m_offsetZero = nullptr;
    m_offsetEdit = m_offsetDown = m_offsetUp = nullptr;
    if (m_memDC)
    {
        ::DeleteDC (m_memDC);
        m_memDC = nullptr;
    }
    if (m_memBmp)
    {
        ::DeleteObject (m_memBmp);
        m_memBmp = nullptr;
    }
    m_memW = m_memH = 0;
    deleteFonts ();
    if (m_bgBrush)
        ::DeleteObject (m_bgBrush);
    m_bgBrush = nullptr;
    if (m_inputBrush)
        ::DeleteObject (m_inputBrush);
    m_inputBrush = nullptr;
}

void WinView::applyPanelSize (int w, int h, bool updateDesign)
{
    clampPanelSize (w, h);
    if (updateDesign)
    {
        // 记住「逻辑尺寸」：跨 DPI 搬窗口时按它换算物理尺寸，不再跳回默认宽度。
        m_designW = static_cast<int> (std::lround (w / m_scale));
        m_designH = static_cast<int> (std::lround (h / m_scale));
    }
    if (w == m_panelW && h == m_panelH)
        return;

    m_panelW = w;
    m_panelH = h;
    if (m_memBmp)
    {
        ::DeleteObject (m_memBmp);
        m_memBmp = nullptr;
        m_memW = m_memH = 0;
    }

    ::SetWindowPos (m_hwnd, nullptr, 0, 0, m_panelW, m_panelH, SWP_NOZORDER | SWP_NOMOVE);
    layoutChildren ();
    m_lastPaintKey = 1.0e30;      // 尺寸变了，下一帧无条件重画
    ::InvalidateRect (m_hwnd, nullptr, TRUE);
    if (m_wave)
        ::InvalidateRect (m_wave, nullptr, FALSE);
}

void WinView::resize (int w, int h)
{
    if (!m_hwnd)
        return;
    applyPanelSize (w, h, true);
}

// 自我防御：宿主给的容器窗口若比我们要的小，就缩回去 —— 否则右侧会整块被父窗口裁掉，
// 「打开音频 / ？帮助 / 回到播放头 / 归零」这些按钮直接看不见（比界面窄更糟）。
// 只有在父窗口真的装不下时才缩；父窗口够大就按我们自己的设计尺寸（用户拖过就是它）。
// 由 50ms 的界面定时器调用（每帧比对一次尺寸，代价可忽略），
// 不碰 m_design —— 父窗口恢复后还能长回用户选择的宽度。
void WinView::syncSizeToParent ()
{
    if (!m_hwnd)
        return;
    const HWND par = ::GetParent (m_hwnd);
    if (!par)
        return;
    RECT rc = {};
    if (!::GetClientRect (par, &rc))
        return;
    const int availW = rc.right - rc.left;
    const int availH = rc.bottom - rc.top;
    if (availW <= 0 || availH <= 0)
        return;

    int wantW = std::min (px (m_designW), availW);
    int wantH = std::min (px (m_designH), availH);
    if (wantW < minPanelW ()) wantW = minPanelW ();
    if (wantH < minPanelH ()) wantH = minPanelH ();

    if (wantW != m_panelW || wantH != m_panelH)
        applyPanelSize (wantW, wantH, false);
}

//------------------------------------------------------------------------------
void WinView::layoutChildren ()
{
    m_layout = ui::arrange (static_cast<int> (std::lround (m_panelW / m_scale)),
                            static_cast<int> (std::lround (m_panelH / m_scale)));
    const auto& l = m_layout;
    auto move = [this] (HWND control, ui::Rect r) {
        if (control)
            ::MoveWindow (control, px (r.x), px (r.y), px (r.w), px (r.h), TRUE);
    };
    m_waveW = px (l.wave.w);
    m_waveH = px (l.wave.h);
    move (m_wave, l.wave);
    move (m_openBtn, l.open);
    move (m_videoBtn, l.video);
    move (m_closeMediaBtn, l.closeMedia);
    move (m_scoreBtn, l.score);
    move (m_themeBtn, l.theme);
    move (m_followScoreCheck, l.followScore);
    move (m_helpBtn, l.help);
    const auto guide = ui::arrangeGuide (l.width, l.height);
    move (m_updateCheck, guide.updateCheck);
    move (m_updateAction, guide.updateAction);
    move (m_collapseBtn, l.collapse);
    move (m_zoomOut, l.zoomOut);
    move (m_zoomIn, l.zoomIn);
    move (m_zoomFit, l.zoomFit);
    move (m_backBtn, l.follow);
    move (m_gridCheck, l.grid);
    move (m_numCheck, l.numbers);
    move (m_offsetZero, l.offsetReset);
    move (m_pitchReset, l.pitchReset);
    move (m_volSlider, l.volumeSlider);
    move (m_progressSlider, l.progress);
    move (m_offsetDown, {l.offset.x + 1, l.offset.y + 1, 24, 30});
    move (m_offsetUp, {l.offset.right () - 25, l.offset.y + 1, 24, 30});
    move (m_offsetEdit, {l.offset.x + 26, l.offset.y + 6, l.offset.w - 52, 20});
    ::SendMessageW (m_offsetEdit, EM_SETLIMITTEXT, 12, 0);

    auto stepper = [&] (ui::Rect r, HWND edit, HWND down, HWND up) {
        move (down, {r.x + 1, r.y + 1, 29, r.h - 2});
        move (up, {r.right () - 30, r.y + 1, 29, r.h - 2});
        move (edit, {r.x + 31, r.y + 6, r.w - 62, 20});
        ::SendMessageW (edit, EM_SETLIMITTEXT, 6, 0);
    };
    stepper (l.bpm, m_bpmEdit, m_bpmDown, m_bpmUp);
    stepper (l.semitone, m_semitoneEdit, m_semitoneDown, m_semitoneUp);
    stepper (l.cents, m_centsEdit, m_centsDown, m_centsUp);
    move (m_timesig, {l.timeSig.x, l.timeSig.y, l.timeSig.w, 240});
    ::SendMessageW (m_timesig, CB_SETITEMHEIGHT, static_cast<WPARAM> (-1), px (26));
    ::SendMessageW (m_timesig, CB_SETITEMHEIGHT, 0, px (28));
    presentMedia ();
}

void WinView::presentMedia ()
{
    const bool show = !m_helpVisible && !m_tsPopupVisible;
    for (HWND control : {m_wave, m_gridCheck, m_numCheck, m_zoomOut, m_zoomIn, m_zoomFit})
        ::ShowWindow (control, show && !m_videoView ? SW_SHOW : SW_HIDE);
    for (HWND control : {m_semitoneEdit, m_semitoneUp, m_semitoneDown,
                         m_centsEdit, m_centsUp, m_centsDown, m_pitchReset})
        ::EnableWindow (control, !m_videoView);
    if (!m_videoView) return;
    if (m_collapsed) layoutFloating ();
    else
    {
        const auto r = m_layout.wave;
        m_videoView->present (m_hwnd, px (r.x), px (r.y), px (r.w), px (r.h), show);
    }
}

double WinView::mediaDuration () const
{
    return m_videoView ? m_videoView->durationSec () : m_backend ? m_backend->durationSec () : 0.0;
}

double WinView::mediaPosition () const
{
    if (!m_backend) return 0.0;
    if (!m_videoView) return m_backend->positionSec ();
    const double seconds = std::max (0.0, m_videoScoreSec + m_backend->offsetSec ());
    const double duration = mediaDuration ();
    return duration > 0 ? std::min (seconds, duration) : seconds;
}

void WinView::paintChrome (HDC dc)
{
    if (!dc)
        return;

    RECT client = {};
    ::GetClientRect (m_hwnd, &client);

    HBRUSH bg = ::CreateSolidBrush (kPaper);
    ::FillRect (dc, &client, bg);
    ::DeleteObject (bg);

    const auto& l = m_layout;
    auto line = [&] (int x, int y, int x2, int y2) {
        ::MoveToEx (dc, px (x), px (y), nullptr);
        ::LineTo (dc, px (x2), px (y2));
    };
    HPEN rule = ::CreatePen (PS_SOLID, std::max (1, px (1)), kSoftRule);
    HGDIOBJ oldPen = ::SelectObject (dc, rule);
    line (20, 60, l.width - 20, 60);
    line (144, 16, 144, 46);
    line (20, l.parametersY, l.width - 20, l.parametersY);
    line (l.pitch.x - 16, l.sync.y + 4, l.pitch.x - 16, l.sync.bottom ());

    // The full input frame belongs to the parent; edit children only paint the value.
    HPEN inputRule = ::CreatePen (PS_SOLID, std::max (1, px (1)), kRule);
    ::SelectObject (dc, inputRule);
    HBRUSH input = ::CreateSolidBrush (kSurface);
    HGDIOBJ oldBrush = ::SelectObject (dc, input);
    for (const ui::Rect r : { l.bpm, l.semitone, l.cents, l.offset })
        ::RoundRect (dc, px (r.x), px (r.y), px (r.right ()), px (r.bottom ()), px (6), px (6));
    ::SelectObject (dc, oldBrush);
    ::DeleteObject (input);
    ::SelectObject (dc, oldPen);
    ::DeleteObject (inputRule);
    ::DeleteObject (rule);

    auto text = [&] (const std::wstring& value, ui::Rect box, HFONT font,
                     COLORREF color, UINT align = DT_LEFT) {
        RECT r = {px (box.x), px (box.y), px (box.right ()), px (box.bottom ())};
        ::SelectObject (dc, font);
        ::SetTextColor (dc, color);
        ::DrawTextW (dc, value.c_str (), -1, &r,
                     align | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
    };
    ::SetBkMode (dc, TRANSPARENT);
    text (L"MuseSync", {20, 18, 116, 28}, m_fontBold, kInk);
    text (m_fileText, l.file, m_font, kInk);
    HBRUSH status = ::CreateSolidBrush (m_backend && m_backend->hostTimeline ().playing ? kAccent : kMuted);
    HGDIOBJ previousBrush = ::SelectObject (dc, status);
    HGDIOBJ previousPen = ::SelectObject (dc, ::GetStockObject (NULL_PEN));
    ::Ellipse (dc, px (l.transport.x), px (l.transport.y + 7), px (l.transport.x + 5), px (l.transport.y + 12));
    ::SelectObject (dc, previousPen);
    ::SelectObject (dc, previousBrush);
    ::DeleteObject (status);
    text (m_transportText, {l.transport.x + 12, l.transport.y, l.transport.w - 12, l.transport.h}, m_fontSmall, kMuted);
    text (m_timeText, l.time, m_fontMono, kMuted);
    auto positionBox = l.musicalPosition;
    positionBox.h = 14;
    text (m_musicalText, positionBox, m_fontHeading, kAccent);
    positionBox.y += 14;
    positionBox.h = 14;
    text (m_scoreTempoText, positionBox, m_fontSmall, kMuted);
    text (L"同步对齐", l.syncTitle, m_fontHeading, kInk);
    text (L"整曲移调", l.pitchTitle, m_fontHeading, kInk);
    text (L"音量", l.volumeLabel, m_fontSmall, kMuted);
    text (L"BPM", l.bpmLabel, m_fontSmall, kMuted);
    text (m_sourceText, l.bpmSource, m_fontSmall, kMuted, DT_RIGHT);
    text (L"拍号", l.timeSigLabel, m_fontSmall, kMuted);
    text (L"半音", l.semitoneLabel, m_fontSmall, kMuted);
    text (L"音分", l.centsLabel, m_fontSmall, kMuted);
    text (L"起始偏移", l.offsetLabel, m_fontSmall, kMuted);
    text (L"ms", l.offsetUnit, m_fontSmall, kMuted);
    text (m_pitchText, l.pitchStatus, m_fontSmall, kMuted, DT_RIGHT);
    text (m_volumeText, l.volumeValue, m_fontSmall, kInk, DT_RIGHT);
    text (m_detectedTempoText, l.detectedTempo, m_fontSmall, kAccent);
    if (m_tempoResult && mediaDuration () > 0)
    {
        const auto box = l.tempoMap;
        const double duration = mediaDuration ();
        auto x = [&] (double t) { return px (box.x + box.w * std::clamp (t / duration, 0.0, 1.0)); };
        HPEN tempoPen = ::CreatePen (PS_SOLID, std::max (1, px (2)), kAccent);
        const auto previous = ::SelectObject (dc, tempoPen);
        bool connected = false;
        double end = -1;
        for (const auto& segment : m_tempoResult->segments)
        {
            if (segment.bpm <= 0) { connected = false; continue; }
            const int y = px (box.bottom () - 2 - (box.h - 4) *
                std::clamp ((segment.bpm - 45) / 195, 0.0, 1.0));
            if (!connected || segment.start > end + .01) ::MoveToEx (dc, x (segment.start), y, nullptr);
            else ::LineTo (dc, x (segment.start), y);
            ::LineTo (dc, std::max (x (segment.start) + 1, x (segment.end)), y);
            connected = true;
            end = segment.end;
        }
        ::SelectObject (dc, previous);
        ::DeleteObject (tempoPen);
        HPEN cursor = ::CreatePen (PS_SOLID, 1, kMuted);
        const auto old = ::SelectObject (dc, cursor);
        ::MoveToEx (dc, x (mediaPosition ()), px (box.y), nullptr);
        ::LineTo (dc, x (mediaPosition ()), px (box.bottom ()));
        ::SelectObject (dc, old);
        ::DeleteObject (cursor);
    }
}

bool WinView::drawItem (const DRAWITEMSTRUCT* dis)
{
    if (!dis)
        return false;

    if (dis->CtlType == ODT_COMBOBOX && dis->CtlID == IDC_TIMESIG_COMBO)
    {
        const bool selected = (dis->itemState & ODS_SELECTED) != 0;
        HBRUSH bg = ::CreateSolidBrush (selected ? kInk : kSurface);
        ::FillRect (dis->hDC, &dis->rcItem, bg);
        ::DeleteObject (bg);
        ::SetBkMode (dis->hDC, TRANSPARENT);
        ::SetTextColor (dis->hDC, selected ? kSurface : kInk);
        ::SelectObject (dis->hDC, m_font);
        if ((m_backend && m_backend->followScore () && (dis->itemState & ODS_COMBOBOXEDIT)) ||
            dis->itemID < static_cast<UINT> (kTimeSigCount))
        {
            RECT r = dis->rcItem;
            r.left += px (10);
            const wchar_t* value = m_backend && m_backend->followScore () &&
                                   (dis->itemState & ODS_COMBOBOXEDIT)
                                       ? m_meterText.c_str () : kTimeSigs[dis->itemID].label;
            ::DrawTextW (dis->hDC, value, -1, &r,
                         DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        }
        return true;
    }
    if (dis->CtlType != ODT_BUTTON)
        return false;

    const bool pressed = (dis->itemState & ODS_SELECTED) != 0;
    const bool focused = (dis->itemState & ODS_FOCUS) != 0;
    const bool hot = m_hotControl && dis->hwndItem == m_hotControl;

    if (dis->CtlID == IDC_GRID_CHECK || dis->CtlID == IDC_NUM_CHECK || dis->CtlID == IDC_FOLLOW_SCORE)
    {
        const bool checked = dis->CtlID == IDC_FOLLOW_SCORE ? m_backend && m_backend->followScore () :
                             dis->CtlID == IDC_GRID_CHECK ? m_showGrid : m_showNumbers;
        HBRUSH bg = ::CreateSolidBrush (pressed || checked || hot ? kAccentSoft : kPaper);
        ::FillRect (dis->hDC, &dis->rcItem, bg);
        ::DeleteObject (bg);
        RECT box = dis->rcItem;
        box.left += px (6);
        box.top += (box.bottom - box.top - px (12)) / 2;
        box.right = box.left + px (12);
        box.bottom = box.top + px (12);

        HBRUSH boxBrush = ::CreateSolidBrush (checked ? kAccent : kSurface);
        HPEN boxPen = ::CreatePen (PS_SOLID, std::max (1, px (1)),
                                   checked ? kAccent : kRule);
        HGDIOBJ oldBrush = ::SelectObject (dis->hDC, boxBrush);
        HGDIOBJ oldPen = ::SelectObject (dis->hDC, boxPen);
        ::Rectangle (dis->hDC, box.left, box.top, box.right, box.bottom);
        ::SelectObject (dis->hDC, oldPen);
        ::SelectObject (dis->hDC, oldBrush);
        ::DeleteObject (boxPen);
        ::DeleteObject (boxBrush);

        if (checked)
        {
            HPEN tickPen = ::CreatePen (PS_SOLID, std::max (1, px (1)), kPaper);
            oldPen = ::SelectObject (dis->hDC, tickPen);
            ::MoveToEx (dis->hDC, box.left + px (2), box.top + px (6), nullptr);
            ::LineTo (dis->hDC, box.left + px (5), box.bottom - px (3));
            ::LineTo (dis->hDC, box.right - px (2), box.top + px (3));
            ::SelectObject (dis->hDC, oldPen);
            ::DeleteObject (tickPen);
        }

        RECT textRc = dis->rcItem;
        textRc.left += px (24);
        ::SetBkMode (dis->hDC, TRANSPARENT);
        ::SetTextColor (dis->hDC, kInk);
        ::SelectObject (dis->hDC, m_fontSmall);
        wchar_t label[64] = {};
        ::GetWindowTextW (dis->hwndItem, label, 64);
        ::DrawTextW (dis->hDC, label, -1, &textRc,
                     DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

        if (focused)
        {
            RECT focus = { dis->rcItem.left + px (1), dis->rcItem.top + px (1),
                           dis->rcItem.right - px (1), dis->rcItem.bottom - px (1) };
            ::DrawFocusRect (dis->hDC, &focus);
        }
        return true;
    }

    COLORREF fill = kPaper;
    COLORREF edge = kPaper;
    COLORREF text = kInk;
    const bool stepper = dis->CtlID == IDC_BPM_UP || dis->CtlID == IDC_BPM_DOWN ||
                         dis->CtlID == IDC_SEMITONE_UP || dis->CtlID == IDC_SEMITONE_DOWN ||
                         dis->CtlID == IDC_CENTS_UP || dis->CtlID == IDC_CENTS_DOWN ||
                         dis->CtlID == IDC_OFFSET_UP || dis->CtlID == IDC_OFFSET_DOWN;
    if (stepper) fill = edge = kSurface;

    const bool primaryOpen = dis->CtlID == IDC_OPEN || dis->CtlID == IDC_OPEN_VIDEO;
    if (primaryOpen)
    {
        fill = kSurface;
        edge = kRule;
    }
    if (hot)
    {
        fill = kAccentSoft;
        edge = primaryOpen ? kAccent : fill;
    }
    if (pressed)
    {
        fill = edge = kSoftRule;
    }

    RECT r = dis->rcItem;
    HBRUSH brush = ::CreateSolidBrush (fill);
    HPEN pen = ::CreatePen (PS_SOLID, std::max (1, px (1)), edge);
    HGDIOBJ oldBrush = ::SelectObject (dis->hDC, brush);
    HGDIOBJ oldPen = ::SelectObject (dis->hDC, pen);
    ::FillRect (dis->hDC, &r, m_bgBrush);
    if (stepper)
        ::Rectangle (dis->hDC, r.left, r.top, r.right, r.bottom);
    else
        ::RoundRect (dis->hDC, r.left, r.top, r.right, r.bottom, px (6), px (6));
    ::SelectObject (dis->hDC, oldPen);
    ::SelectObject (dis->hDC, oldBrush);
    ::DeleteObject (pen);
    ::DeleteObject (brush);

    wchar_t label[128] = {};
    ::GetWindowTextW (dis->hwndItem, label, 128);
    const wchar_t* icon = nullptr;
    switch (dis->CtlID)
    {
        case IDC_THEME_BTN: icon = L"\xE790"; break;
        case IDC_ZOOM_OUT: icon = L"\xE71F"; break;
        case IDC_ZOOM_IN: icon = L"\xE8A3"; break;
        case IDC_ZOOM_FIT: icon = L"\xE740"; break;
        case IDC_BACK_TO_SCORE: icon = L"\xE81D"; break;
        case IDC_HELP_BTN: icon = L"\xE897"; break;
        case IDC_HELP_CLOSE: icon = L"\xE8BB"; break;
        case IDC_CLOSE_MEDIA: icon = L"\xE8BB"; break;
        case IDC_COLLAPSE: icon = L"\xE921"; break;
        case IDC_EXPAND: icon = L"\xE8A7"; break;
        case IDC_OFFSET_ZERO:
        case IDC_PITCH_RESET: icon = L"\xE72C"; break;
        case IDC_BPM_UP:
        case IDC_OFFSET_UP:
        case IDC_SEMITONE_UP:
        case IDC_CENTS_UP: icon = L"\xE710"; break;
        case IDC_BPM_DOWN:
        case IDC_OFFSET_DOWN:
        case IDC_SEMITONE_DOWN:
        case IDC_CENTS_DOWN: icon = L"\xE738"; break;
    }
    ::SetBkMode (dis->hDC, TRANSPARENT);
    ::SetTextColor (dis->hDC, (dis->itemState & ODS_DISABLED) ? kMuted : text);
    ::SelectObject (dis->hDC, icon ? m_fontIcons : m_font);
    ::DrawTextW (dis->hDC, icon ? icon : label, -1, &r,
                 DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    if (dis->CtlID == IDC_HELP_BTN && (m_updateStatus.phase == updates::Phase::available ||
        m_updateStatus.phase == updates::Phase::ready))
    {
        HBRUSH dot = ::CreateSolidBrush (kUpdateAlert);
        RECT badge {r.right - px (8), r.top + px (3), r.right - px (3), r.top + px (8)};
        HGDIOBJ oldBrush = ::SelectObject (dis->hDC, dot);
        HGDIOBJ oldPen = ::SelectObject (dis->hDC, ::GetStockObject (NULL_PEN));
        ::Ellipse (dis->hDC, badge.left, badge.top, badge.right, badge.bottom);
        ::SelectObject (dis->hDC, oldPen);
        ::SelectObject (dis->hDC, oldBrush);
        ::DeleteObject (dot);
    }

    if (focused)
    {
        RECT focus = { r.left + px (3), r.top + px (3), r.right - px (3), r.bottom - px (3) };
        ::DrawFocusRect (dis->hDC, &focus);
    }
    return true;
}

//------------------------------------------------------------------------------
double WinView::xToSec (int x) const
{
    // 基准必须是【缩放后】的波形区宽度，否则 DPI ≠ 96 时点击定位会整体偏移。
    return m_viewStart + (static_cast<double> (x) / m_waveW) * m_viewSpan;
}

int WinView::secToX (double sec) const
{
    if (m_viewSpan <= 0.0)
        return 0;
    return static_cast<int> ((sec - m_viewStart) / m_viewSpan * m_waveW);
}

//------------------------------------------------------------------------------
void WinView::paint (HDC target, const RECT& rc)
{
    // 【恒按整块波形区绘制】secToX() / xToSec() 的基准是 m_waveW，
    // 所以离屏位图也必须恒为 m_waveW × m_waveH；最后再把 rc 与波形区的
    // 交集 Blt 上屏。若按 rc 的宽高建位图，局部重绘（rcPaint 非全窗）时
    // 波形/网格/播放头会整块错位裁切。
    const int W = m_waveW;
    const int H = m_waveH;
    // 波形绘制区的上边界：其上是拍号条（见 kTsStripH 的说明）。
    // 视图/窗口尺寸都没变，只是把顶部一小条分给了拍号条 —— 所以布局与验证器不用动。
    const int waveTop = px (kTsStripH);

    const int sx = std::max (0, static_cast<int> (rc.left));
    const int sy = std::max (0, static_cast<int> (rc.top));
    const int sw = std::min (W, static_cast<int> (rc.right))  - sx;
    const int sh = std::min (H, static_cast<int> (rc.bottom)) - sy;
    if (sw <= 0 || sh <= 0)
        return;

    // 离屏位图【复用】：旧版每次 paint 都 CreateCompatibleDC + CreateCompatibleBitmap，
    // 拖波形时等于每帧一次 GDI 分配 —— 在虚拟机（软件 GDI、vCPU 又少）上足够把
    // 音频线程饿出爆音（用户实测「边播放边拖波形，音频变慢」）。
    if (!m_memDC)
        m_memDC = ::CreateCompatibleDC (target);
    if (!m_memDC)
        return;
    if (!m_memBmp || m_memW != W || m_memH != H)
    {
        if (m_memBmp)
            ::DeleteObject (m_memBmp);
        m_memBmp = ::CreateCompatibleBitmap (target, W, H);
        m_memW = W;
        m_memH = H;
    }
    if (!m_memBmp)
    {
        ::DeleteDC (m_memDC);
        m_memDC = nullptr;
        return;
    }
    HDC dc = m_memDC;
    HBITMAP oldBmp = reinterpret_cast<HBITMAP> (::SelectObject (dc, m_memBmp));

    RECT full = { 0, 0, W, H };
    HBRUSH bg = ::CreateSolidBrush (kSurface);
    ::FillRect (dc, &full, bg);
    ::DeleteObject (bg);

    if (m_backend && !m_backend->hasAudio ())
    {
        HPEN center = ::CreatePen (PS_SOLID, 1, kSoftRule);
        HGDIOBJ oldPen = ::SelectObject (dc, center);
        const int mid = waveTop + (H - waveTop) / 2;
        ::MoveToEx (dc, 0, mid, nullptr);
        ::LineTo (dc, W, mid);
        ::SelectObject (dc, oldPen);
        ::DeleteObject (center);
        ::SetBkMode (dc, TRANSPARENT);
        ::SetTextColor (dc, kMuted);
        ::SelectObject (dc, m_font);
        RECT tr = { px (12), mid - px (42), W - px (12), mid - px (12) };
        ::DrawTextW (dc, L"未载入音频", -1, &tr,
                     DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    else if (m_backend)
    {
        const float offset = m_backend->offsetSec ();
        // Paint the skipped intro beneath the waveform, grid and playheads.
        if (offset > m_viewStart)
        {
            const int x = secToX (offset);
            if (x > 0)
            {
                RECT shade = {0, waveTop, std::min (x, W), H};
                HBRUSH brush = ::CreateSolidBrush (kAccentSoft);
                ::FillRect (dc, &shade, brush);
                ::DeleteObject (brush);
            }
        }
        // ---- 波形 ----
        std::vector<float> peaks;
        m_backend->waveformPeaks (peaks, W, m_viewStart, m_viewStart + m_viewSpan);
        if (!peaks.empty ())
        {
            HPEN wavePen = ::CreatePen (PS_SOLID, std::max (1, px (1)), kMuted);
            HPEN oldPen = reinterpret_cast<HPEN> (::SelectObject (dc, wavePen));
            const int mid = waveTop + (H - waveTop) / 2;
            const int n = static_cast<int> (peaks.size ());
            for (int i = 0; i < n && i < W; ++i)
            {
                float p = peaks[static_cast<size_t> (i)];
                if (p < 0.0f) p = 0.0f;
                if (p > 1.0f) p = 1.0f;
                const int half = static_cast<int> (p * ((H - waveTop) / 2 - px (2)));
                ::MoveToEx (dc, i, mid - half, nullptr);
                ::LineTo (dc, i, mid + half + 1);
            }
            ::SelectObject (dc, oldPen);
            ::DeleteObject (wavePen);
        }

        // ---- 小节网格 ----
        // ⚠️ 小节长度【逐小节算】：谱子里有拍号变化时，从变化点起每小节的长度就
        //    不一样了（4/4 = 2 秒、转 3/4 之后 = 1.5 秒）。用全曲统一的 barSec 铺线，
        //    变化点之后的小节线会全部错位。推进算法 = gui.h 的 ap::buildBars()，
        //    三端共用同一份（别在这里另写一套）。
        const PlugView::Backend::HostTimeline tl0 = m_backend->hostTimeline ();

        std::vector<ap::BarMark> bars;
        rebuildBars (bars);

        if (m_showGrid && !bars.empty ())
        {
            // ⚠️ PS_DOT / PS_DASH 只支持宽度 1（几何画笔限制），所以拍线不跟着放大，
            // 小节线是实线、可以放大。这样高 DPI 下小节线更醒目，层次反而更清楚。
            HPEN barPen  = ::CreatePen (PS_SOLID, std::max (1, px (1)), kRule);
            HPEN beatPen = ::CreatePen (PS_DOT,   1, kSoftRule);
            HPEN prevPen = reinterpret_cast<HPEN> (::SelectObject (dc, barPen));
            ::SetBkMode (dc, TRANSPARENT);

            const double view1 = m_viewStart + m_viewSpan;

            for (const ap::BarMark& m : bars)
            {
                // 拍线（虚线）：太密就不画，只留小节线，免得糊成一片
                if (m.beatSec * static_cast<double> (W) / m_viewSpan >= px (5))
                {
                    ::SelectObject (dc, beatPen);
                    for (int q = 1; q < m.beats; ++q)
                    {
                        const double t = m.startSec + (m.beatOffsets.empty () ? q * m.beatSec :
                            q < static_cast<int> (m.beatOffsets.size ()) ? m.beatOffsets[q] : m.durationSec);
                        if (t >= m.startSec + m.durationSec - 1e-9) break;
                        if (t < m_viewStart || t > view1)
                            continue;
                        const int x = secToX (t);
                        if (x < 0 || x > W)
                            continue;
                        ::MoveToEx (dc, x, waveTop, nullptr);
                        ::LineTo (dc, x, H);
                    }
                }

                // 小节线（实线）
                if (m.startSec < m_viewStart || m.startSec > view1)
                    continue;
                const int x = secToX (m.startSec);
                if (x < 0 || x > W)
                    continue;
                ::SelectObject (dc, barPen);
                ::MoveToEx (dc, x, waveTop, nullptr);
                ::LineTo (dc, x, H);
            }

            ::SelectObject (dc, prevPen);
            ::DeleteObject (barPen);
            ::DeleteObject (beatPen);
        }

        // 小节号：独立开关，不依赖网格线。
        // ⚠️ 小节宽度可变后「固定步长」不再适用 —— 改成「与上一个标注至少隔开
        //    42 设计像素才标」，宽窄自动适应。
        if (m_showNumbers && !bars.empty ())
        {
            ::SelectObject (dc, m_fontSmall);
            ::SetTextColor (dc, kMuted);
            const double view1 = m_viewStart + m_viewSpan;
            int lastLabelX = -1000000;
            for (const ap::BarMark& m : bars)
            {
                if (m.startSec < m_viewStart || m.startSec > view1)
                    continue;
                const int x = secToX (m.startSec);
                if (x < -px (20) || x > W)
                    continue;
                if (x - lastLabelX < px (42))
                    continue;
                lastLabelX = x;
                wchar_t buf[32];
                std::swprintf (buf, 32, L"%d", m.number);
                ::TextOutW (dc, x + px (2), waveTop + px (2), buf,
                            static_cast<int> (std::wcslen (buf)));
            }
        }

        // ---- 顶部拍号条 ----
        drawTimeSigStrip (dc, tsStripBoxes ());

        // ---- 谱面播放头（绿色）----
        if (tl0.playheadValid)
        {
            const int x = secToX (tl0.playheadSec + offset);
            if (x >= 0 && x <= W)
            {
                HPEN p = ::CreatePen (PS_DASH, 1, kOlive);
                HPEN op = reinterpret_cast<HPEN> (::SelectObject (dc, p));
                ::MoveToEx (dc, x, waveTop, nullptr);
                ::LineTo (dc, x, H);
                ::SelectObject (dc, op);
                ::DeleteObject (p);
                HBRUSH marker = ::CreateSolidBrush (kOlive);
                RECT flag {std::max (0, x - px (4)), H - px (7), std::min (W, x + px (5)), H};
                ::FillRect (dc, &flag, marker);
                ::DeleteObject (marker);
            }
        }

        // ---- 音频播放头（红色）----
        {
            const int x = secToX (m_backend->positionSec ());
            if (x >= 0 && x <= W)
            {
                HPEN p = ::CreatePen (PS_SOLID, std::max (1, px (2)), kAccentDark);
                HPEN op = reinterpret_cast<HPEN> (::SelectObject (dc, p));
                ::MoveToEx (dc, x, waveTop, nullptr);
                ::LineTo (dc, x, H);
                ::SelectObject (dc, op);
                ::DeleteObject (p);
                POINT points[] = {{x - px (5), waveTop}, {x + px (5), waveTop}, {x, waveTop + px (7)}};
                HBRUSH marker = ::CreateSolidBrush (kAccentDark);
                HGDIOBJ previousBrush = ::SelectObject (dc, marker);
                HGDIOBJ previousPen = ::SelectObject (dc, ::GetStockObject (NULL_PEN));
                ::Polygon (dc, points, 3);
                ::SelectObject (dc, previousPen);
                ::SelectObject (dc, previousBrush);
                ::DeleteObject (marker);
            }
        }

        // ---- 分家检测徽标 ----
        // 只在「播放中、且不在首尾过渡区」判定：
        //   · 起播头 0.3 秒音频还在追，差值天然偏大 → 不算；
        //   · 音频比乐谱短时尾端必然拉开 → 不算（那不是 bug）。
        // 命中就亮一个红底徽标，把「用户肉眼看不出来的分家」变成一句明确指令。
        {
            const double dur = m_backend->durationSec ();
            const double pos = m_backend->positionSec ();
            const bool mid = (pos > 0.30 && pos < dur - 0.30);
            if (tl0.playing && tl0.playheadValid && dur > 0.60 && mid)
            {
                const double diff = pos - (tl0.playheadSec + offset);
                if (std::fabs (diff) > kDivergenceWarnSec)
                {
                    // 刚点过「回到播放头」却还是错位 → 不是一次性偏差，而是跟随失效，
                    // 此时再点按钮也没用，直接告诉用户重启插件（onTimer 里已记日志）。
                    wchar_t warn[128];
                    if (m_followFail)
                        std::swprintf (warn, 128,
                                       L"跟随可能已失效 · 建议重启插件（错位 %.2f 秒）",
                                       std::fabs (diff));
                    else
                        std::swprintf (warn, 128,
                                       L"与谱面错位 %.2f 秒 · 点「回到播放头」",
                                       std::fabs (diff));

                    ::SelectObject (dc, m_fontSmall);
                    ::SetBkMode (dc, TRANSPARENT);
                    SIZE sz = {};
                    ::GetTextExtentPoint32W (dc, warn,
                                             static_cast<int> (std::wcslen (warn)), &sz);

                    RECT box = { W - sz.cx - px (16), waveTop + px (4),
                                 W - px (4), waveTop + px (4) + sz.cy + px (6) };
                    if (box.left < 0) box.left = 0;
                    HBRUSH bw = ::CreateSolidBrush (kAccentDark);
                    ::FillRect (dc, &box, bw);
                    ::DeleteObject (bw);

                    ::SetTextColor (dc, RGB (255, 255, 255));
                    RECT tr = box;
                    tr.left += px (4);
                    ::DrawTextW (dc, warn, -1, &tr,
                                 DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
                }
            }
        }
    }

    // ⚠️ 顺序绝不能反：BitBlt 读的是 DC 【当前选中】的那张位图。
    // 先 SelectObject (dc, oldBmp) 会把 CreateCompatibleDC 自带的 1×1 单色
    // 占位图挂回去 —— 拷出来的就是一整块纯色，画进 bmp 的波形/网格/
    // 播放头全部不上屏（这正是「波形区一片空白」的根因）。
    // 位图本体【不删】—— 它是复用的（见函数开头）。
    ::BitBlt (target, rc.left, rc.top, sw, sh, dc, sx, sy, SRCCOPY);
    ::SelectObject (dc, oldBmp);
}

//------------------------------------------------------------------------------
// Floating strip keeps the editor and its timer alive while the host shell is hidden.
void WinView::setCollapsed (bool collapsed)
{
    if (!m_hwnd || m_collapsed == collapsed) return;
    if (collapsed)
    {
        ::SetFocus (m_hwnd);
        RECT anchor {};
        ::GetWindowRect (m_collapseBtn, &anchor);
        const HWND root = ::GetAncestor (m_hwnd, GA_ROOT);
        // Only hide an owned editor dialog, never the application's main window.
        wchar_t title[128] {};
        ::GetWindowTextW (root, title, 128);
        m_editorShell = root != m_hwnd && (::GetWindow (root, GW_OWNER) ||
                                          (std::wcscmp (title, L"MuseSync") == 0 ||
                                           std::wcscmp (title, L"MuseTone") == 0)) ? root : nullptr;
        if (!m_floating)
        {
            HWND owner = m_editorShell ? ::GetWindow (m_editorShell, GW_OWNER) : root;
            m_floating = ::CreateWindowExW (WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
                kFloatingClass, L"MuseSync", WS_POPUP | WS_CLIPCHILDREN,
                anchor.left, anchor.top, px (260), px (58), owner, nullptr,
                ::GetModuleHandleW (nullptr), this);
            if (!m_floating) return;
            if (m_videoView)
                ::SetWindowLongPtrW (m_floating, GWL_STYLE,
                    WS_POPUP | WS_THICKFRAME | WS_CLIPCHILDREN);
            m_expandBtn = ::CreateWindowExW (0, L"BUTTON", L"展开 MuseSync",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                px (220), px (11), px (32), px (36), m_floating,
                reinterpret_cast<HMENU> (IDC_EXPAND), ::GetModuleHandleW (nullptr), nullptr);
            ::SetWindowSubclass (m_expandBtn, spaceSyncSubclassProc, kSpaceSyncSubclassId,
                                  reinterpret_cast<DWORD_PTR> (this));
            if (m_tooltip && m_expandBtn)
            {
                TOOLINFOW tool {};
                tool.cbSize = sizeof tool;
                tool.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
                tool.hwnd = m_floating;
                tool.uId = reinterpret_cast<UINT_PTR> (m_expandBtn);
                tool.lpszText = const_cast<wchar_t*> (L"展开 MuseSync");
                ::SendMessageW (m_tooltip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM> (&tool));
            }
        }
        MONITORINFO monitor {sizeof monitor};
        ::GetMonitorInfoW (::MonitorFromRect (&anchor, MONITOR_DEFAULTTONEAREST), &monitor);
        m_shellWasVisible = m_editorShell && ::IsWindowVisible (m_editorShell);
        m_floatingText = m_musicalText + L"  " + m_transportText + L"  " + m_timeText;
        m_collapsed = true;
        const int floatingW = m_videoView ? 640 : 260;
        const int floatingH = m_videoView ? 412 : 58;
        ::SetWindowLongPtrW (m_floating, GWL_STYLE,
            WS_POPUP | WS_CLIPCHILDREN | (m_videoView ? WS_THICKFRAME : 0));
        ::SetWindowPos (m_floating, nullptr, 0, 0, px (floatingW), px (floatingH),
                        SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
        if (m_videoView)
        {
            for (HWND control : {m_progressSlider, m_volSlider, m_offsetEdit, m_offsetDown, m_offsetUp})
                ::SetParent (control, m_floating);
            if (!m_opacitySlider)
            {
                m_opacitySlider = ::CreateWindowExW (0, TRACKBAR_CLASSW, L"不透明度",
                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | TBS_HORZ | TBS_NOTICKS,
                    0, 0, 10, 10, m_floating, reinterpret_cast<HMENU> (IDC_VIDEO_OPACITY),
                    ::GetModuleHandleW (nullptr), nullptr);
                ::SendMessageW (m_opacitySlider, TBM_SETRANGE, TRUE, MAKELPARAM (20, 100));
                ::SendMessageW (m_opacitySlider, TBM_SETPOS, TRUE, m_videoOpacity);
                ::SetWindowSubclass (m_opacitySlider, spaceSyncSubclassProc, kSpaceSyncSubclassId,
                                     reinterpret_cast<DWORD_PTR> (this));
                TOOLINFOW tool {sizeof tool};
                tool.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
                tool.hwnd = m_floating;
                tool.uId = reinterpret_cast<UINT_PTR> (m_opacitySlider);
                tool.lpszText = const_cast<wchar_t*> (L"视频悬浮窗不透明度");
                ::SendMessageW (m_tooltip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM> (&tool));
            }
            ::ShowWindow (m_opacitySlider, SW_SHOW);
            layoutFloating ();
            setVideoOpacity ();
        }
        else
        {
            ::SetWindowRgn (m_floating, nullptr, TRUE);
            ::SetWindowLongPtrW (m_floating, GWL_EXSTYLE,
                ::GetWindowLongPtrW (m_floating, GWL_EXSTYLE) & ~WS_EX_LAYERED);
            ::ShowWindow (m_opacitySlider, SW_HIDE);
            ::MoveWindow (m_expandBtn, px (220), px (11), px (32), px (36), TRUE);
        }
        ::ShowWindow (m_hwnd, SW_HIDE);
        if (m_shellWasVisible) ::ShowWindow (m_editorShell, SW_HIDE);
        // Do not make an invisible test host (or a hidden host view) appear.
        const bool visible = m_shellWasVisible || (!m_editorShell && ::IsWindowVisible (root));
        // Use the actual toolbar position, including the video window's resize border.
        RECT expand {}, window {};
        ::GetWindowRect (m_expandBtn, &expand);
        ::GetWindowRect (m_floating, &window);
        const int x = window.left + (anchor.left + anchor.right - expand.left - expand.right) / 2;
        const int y = window.top + (anchor.top + anchor.bottom - expand.top - expand.bottom) / 2;
        const int placedX = std::clamp<LONG> (x, monitor.rcWork.left,
            std::max (monitor.rcWork.left, monitor.rcWork.right - px (floatingW)));
        const int placedY = std::clamp<LONG> (y, monitor.rcWork.top,
            std::max (monitor.rcWork.top, monitor.rcWork.bottom - px (floatingH)));
        ::SetWindowPos (m_floating, HWND_TOPMOST, placedX, placedY, px (floatingW), px (floatingH),
                        SWP_NOACTIVATE | (visible ? SWP_SHOWWINDOW : 0));
        if (m_videoView) layoutFloating ();
    }
    else
    {
        m_collapsed = false;
        ::ShowWindow (m_floating, SW_HIDE);
        for (HWND control : {m_progressSlider, m_volSlider, m_offsetEdit, m_offsetDown, m_offsetUp})
            if (::GetParent (control) == m_floating) ::SetParent (control, m_hwnd);
        layoutChildren ();
        ::ShowWindow (m_hwnd, SW_SHOW);
        if (m_shellWasVisible && ::IsWindow (m_editorShell))
        {
            ::ShowWindow (m_editorShell, SW_SHOW);
            ::SetForegroundWindow (m_editorShell);
        }
        ::SetFocus (m_hwnd);
        ::RedrawWindow (m_hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
    }
}

LRESULT WinView::floatingHitTest (POINT screen) const
{
    for (HWND control : {m_expandBtn, m_progressSlider, m_opacitySlider,
                         m_volSlider, m_offsetEdit, m_offsetDown, m_offsetUp})
    {
        if (!control || ::GetParent (control) != m_floating) continue;
        RECT r {};
        ::GetWindowRect (control, &r);
        if (::PtInRect (&r, screen)) return HTCLIENT;
    }
    if (m_videoView)
    {
        RECT r {};
        ::GetClientRect (m_floating, &r);
        ::MapWindowPoints (m_floating, nullptr, reinterpret_cast<POINT*> (&r), 2);
        const int border = px (6);
        const bool left = screen.x < r.left + border, right = screen.x >= r.right - border;
        const bool top = screen.y < r.top + border, bottom = screen.y >= r.bottom - border;
        if (top) return left ? HTTOPLEFT : right ? HTTOPRIGHT : HTTOP;
        if (bottom) return left ? HTBOTTOMLEFT : right ? HTBOTTOMRIGHT : HTBOTTOM;
        if (left) return HTLEFT;
        if (right) return HTRIGHT;
        const auto l = ui::arrangeVideoFloating (
            static_cast<int> (std::lround ((r.right - r.left) / m_scale)),
            static_cast<int> (std::lround ((r.bottom - r.top) / m_scale)));
        if (screen.y >= r.top + px (l.video.y)) return HTTRANSPARENT;
    }
    RECT expand {};
    ::GetWindowRect (m_expandBtn, &expand);
    return ::PtInRect (&expand, screen) ? HTCLIENT : HTCAPTION;
}

void WinView::layoutFloating ()
{
    if (!m_collapsed || !m_videoView || !m_floating) return;
    RECT rc {};
    ::GetClientRect (m_floating, &rc);
    const auto l = ui::arrangeVideoFloating (
        static_cast<int> (std::lround (rc.right / m_scale)),
        static_cast<int> (std::lround (rc.bottom / m_scale)));
    auto move = [this] (HWND control, ui::Rect r) {
        if (control) ::MoveWindow (control, px (r.x), px (r.y), px (r.w), px (r.h), TRUE);
    };
    move (m_expandBtn, l.expand);
    move (m_progressSlider, l.progress);
    move (m_opacitySlider, l.opacity);
    move (m_volSlider, l.volume);
    move (m_offsetDown, {l.offset.x + 1, l.offset.y + 1, 24, 30});
    move (m_offsetUp, {l.offset.right () - 25, l.offset.y + 1, 24, 30});
    move (m_offsetEdit, {l.offset.x + 26, l.offset.y + 6, l.offset.w - 52, 20});
    const auto r = l.video;
    // Keep a resize frame around the mouse-transparent video cutout.
    HRGN region = ::CreateRectRgn (0, 0, rc.right, rc.bottom);
    HRGN cutout = ::CreateRectRgn (px (r.x), px (r.y), px (r.right ()), px (r.bottom ()));
    if (region && cutout) ::CombineRgn (region, region, cutout, RGN_DIFF);
    if (cutout) ::DeleteObject (cutout);
    HRGN previous = ::CreateRectRgn (0, 0, 0, 0);
    const bool unchanged = previous && ::GetWindowRgn (m_floating, previous) != ERROR &&
        ::EqualRgn (region, previous);
    if (previous) ::DeleteObject (previous);
    if (region && (unchanged || !::SetWindowRgn (m_floating, region, TRUE))) ::DeleteObject (region);
    m_videoView->present (m_floating, px (r.x), px (r.y), px (r.w), px (r.h),
        ::IsWindowVisible (m_floating) != FALSE, true, m_videoOpacity);
    ::InvalidateRect (m_floating, nullptr, FALSE);
}

void WinView::moveFloatingVideo ()
{
    if (!m_collapsed || !m_videoView || !m_floating) return;
    RECT rc {};
    ::GetClientRect (m_floating, &rc);
    const auto l = ui::arrangeVideoFloating (
        static_cast<int> (std::lround (rc.right / m_scale)),
        static_cast<int> (std::lround (rc.bottom / m_scale)));
    const auto r = l.video;
    m_videoView->present (m_floating, px (r.x), px (r.y), px (r.w), px (r.h),
        ::IsWindowVisible (m_floating) != FALSE, true, m_videoOpacity);
}

void WinView::setVideoOpacity ()
{
    if (!m_opacitySlider || !m_floating) return;
    m_videoOpacity = static_cast<int> (::SendMessageW (m_opacitySlider, TBM_GETPOS, 0, 0));
    ::SetWindowLongPtrW (m_floating, GWL_EXSTYLE,
        ::GetWindowLongPtrW (m_floating, GWL_EXSTYLE) | WS_EX_LAYERED);
    ::SetLayeredWindowAttributes (m_floating, 0,
        static_cast<BYTE> (std::clamp (m_videoOpacity, 20, 100) * 255 / 100), LWA_ALPHA);
    layoutFloating ();
    ::InvalidateRect (m_floating, nullptr, FALSE);
}

void WinView::paintFloating (HDC dc)
{
    RECT r {};
    ::GetClientRect (m_floating, &r);
    ::FillRect (dc, &r, m_bgBrush);
    HBRUSH edge = ::CreateSolidBrush (kRule);
    ::FrameRect (dc, &r, edge);
    ::DeleteObject (edge);
    ::SetBkMode (dc, TRANSPARENT);
    if (m_videoView)
    {
        const auto l = ui::arrangeVideoFloating (
            static_cast<int> (std::lround (r.right / m_scale)),
            static_cast<int> (std::lround (r.bottom / m_scale)));
        RECT videoFrame {px (l.video.x) - 1, px (l.video.y) - 1,
                         px (l.video.right ()) + 1, px (l.video.bottom ()) + 1};
        HBRUSH videoEdge = ::CreateSolidBrush (kRule);
        ::FrameRect (dc, &videoFrame, videoEdge);
        ::DeleteObject (videoEdge);
        RECT position {px (l.position.x), px (l.position.y), px (l.position.right ()), px (l.position.y + 14)};
        ::SelectObject (dc, m_fontHeading);
        ::SetTextColor (dc, kAccent);
        ::DrawTextW (dc, m_musicalText.c_str (), -1, &position,
            DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
        RECT scoreTempo {px (l.position.x), px (l.position.y + 14), px (l.position.right ()), px (l.position.bottom ())};
        ::SelectObject (dc, m_fontSmall);
        ::SetTextColor (dc, kMuted);
        ::DrawTextW (dc, m_scoreTempoText.c_str (), -1, &scoreTempo,
            DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
        RECT time {px (l.time.x), px (l.time.y), px (l.time.right ()), px (l.time.bottom ())};
        ::SelectObject (dc, m_fontMono);
        ::SetTextColor (dc, kMuted);
        ::DrawTextW (dc, m_timeText.c_str (), -1, &time,
            DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
        RECT icon {px (l.opacityIcon.x), px (l.opacityIcon.y), px (l.opacityIcon.right ()), px (l.opacityIcon.bottom ())};
        ::SelectObject (dc, m_fontIcons);
        ::SetTextColor (dc, kMuted);
        ::DrawTextW (dc, L"\xE793", -1, &icon, DT_SINGLELINE | DT_VCENTER | DT_CENTER);
        RECT value {px (l.opacityValue.x), px (l.opacityValue.y),
                    px (l.opacityValue.right ()), px (l.opacityValue.bottom ())};
        wchar_t opacity[16] {};
        std::swprintf (opacity, 16, L"%d%%", m_videoOpacity);
        ::SelectObject (dc, m_fontSmall);
        ::DrawTextW (dc, opacity, -1, &value, DT_SINGLELINE | DT_VCENTER | DT_RIGHT | DT_NOPREFIX);
        auto label = [&] (ui::Rect box, const wchar_t* text, HFONT font) {
            RECT bounds {px (box.x), px (box.y), px (box.right ()), px (box.bottom ())};
            ::SelectObject (dc, font);
            ::DrawTextW (dc, text, -1, &bounds, DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_NOPREFIX);
        };
        label (l.volumeIcon, L"\xE767", m_fontIcons);
        label (l.volumeValue, m_volumeText.c_str (), m_fontSmall);
        label (l.offsetLabel, L"\xE916", m_fontIcons);
        label (l.offsetUnit, L"ms", m_fontSmall);
        RECT offset {px (l.offset.x), px (l.offset.y), px (l.offset.right ()), px (l.offset.bottom ())};
        ::FillRect (dc, &offset, m_inputBrush);
        HBRUSH border = ::CreateSolidBrush (kRule);
        ::FrameRect (dc, &offset, border);
        ::DeleteObject (border);
        return;
    }
    ::SelectObject (dc, m_fontHeading);
    ::SetTextColor (dc, kInk);
    RECT title {px (14), px (7), px (112), px (28)};
    ::DrawTextW (dc, L"MuseSync", -1, &title, DT_SINGLELINE | DT_VCENTER);
    ::SelectObject (dc, m_fontHeading);
    ::SetTextColor (dc, kAccent);
    RECT position {px (114), px (3), px (208), px (17)};
    ::DrawTextW (dc, m_musicalText.c_str (), -1, &position,
                 DT_SINGLELINE | DT_VCENTER | DT_RIGHT | DT_END_ELLIPSIS | DT_NOPREFIX);
    ::SelectObject (dc, m_fontSmall);
    ::SetTextColor (dc, kMuted);
    RECT scoreTempo {px (114), px (17), px (208), px (31)};
    ::DrawTextW (dc, m_scoreTempoText.c_str (), -1, &scoreTempo,
                 DT_SINGLELINE | DT_VCENTER | DT_RIGHT | DT_END_ELLIPSIS | DT_NOPREFIX);
    RECT status {px (14), px (30), px (94), px (49)};
    ::DrawTextW (dc, m_transportText.c_str (), -1, &status,
                 DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
    ::SelectObject (dc, m_fontMono);
    RECT time {px (98), px (30), px (208), px (49)};
    ::DrawTextW (dc, m_timeText.c_str (), -1, &time,
                 DT_SINGLELINE | DT_VCENTER | DT_RIGHT | DT_END_ELLIPSIS | DT_NOPREFIX);
}

// 使用指南
//
// Two-column guide is drawn by the parent while normal controls are hidden.
// Close explicitly with the top-right icon or Escape so reading clicks are harmless.
//
// ⚠️ 【为什么不是独立子窗口】旧版把指南做成一个铺满面板的 kHelpClass 子窗口，
//    依赖「最后一个创建 = z 序最上」。用户实测在 MuseScore 里失效：点「？帮助」
//    之后主界面没被盖住，指南反而画在了它【下面】——看起来像没反应。
//    根因在宿主的窗口/合成层级上，靠 SetWindowPos(HWND_TOP) 扳不动。
//    现在的做法是把这一类问题从根上删掉：显示指南时隐藏【所有】控件窗口，
//    由主窗口在自己的 WM_PAINT 里整块画 —— 层级不参与，就不会错。
//    （Linux 端本来就是这么实现的：整个界面都画在离屏 pixmap 上。）
//------------------------------------------------------------------------------
void WinView::toggleHelp ()
{
    setHelp (!m_helpVisible);
}

void WinView::hideHelp ()
{
    setHelp (false);
}

void WinView::setHelp (bool on)
{
    if (m_helpVisible == on)
        return;
    m_helpVisible = on;

    showControls (!on);
    ::ShowWindow (m_updateCheck, on ? SW_SHOW : SW_HIDE);
    ::ShowWindow (m_updateAction, on ? SW_SHOW : SW_HIDE);

    if (on && m_hwnd)
    {
        // 打开指南时若输入框还握着焦点，先让它失焦提交（否则隐藏窗口会触发
        // EN_KILLFOCUS 在隐藏之后才提交，顺序拧着），再重画整块面板。
        if (m_backend && m_bpmEdit && ::GetFocus () == m_bpmEdit)
            applyBpmFromEdit ();
        ::SetFocus (m_hwnd);
    }

    ::InvalidateRect (m_hwnd, nullptr, TRUE);
}

void WinView::showControls (bool show)
{
    const HWND all[] = {
        m_wave, m_openBtn, m_videoBtn, m_closeMediaBtn, m_scoreBtn, m_themeBtn, m_followScoreCheck, m_collapseBtn,
        m_helpBtn, m_zoomOut, m_zoomIn, m_zoomFit, m_backBtn,
        m_gridCheck, m_numCheck, m_bpmEdit, m_bpmUp, m_bpmDown, m_timesig,
        m_volSlider, m_progressSlider, m_offsetZero, m_offsetEdit, m_offsetDown, m_offsetUp, m_semitoneEdit, m_semitoneUp, m_semitoneDown,
        m_centsEdit, m_centsUp, m_centsDown, m_pitchReset
    };
    for (HWND h : all)
        if (h)
            ::ShowWindow (h, show ? SW_SHOW : SW_HIDE);
    presentMedia ();
}

void WinView::paintHelp (HDC dc)
{
    struct Section { const wchar_t* title; const wchar_t* body; };
    static const Section guide[] = {
        {L"载入音频与视频", L"打开或拖入 MP3 / WAV / M4A / AAC / FLAC / OGG。\n点击「打开视频」可载入 MP4 / WebM / M4V，或输入网络视频网址（支持 Bilibili 页面）。\n视频窗口需要 Microsoft Edge WebView2 Runtime。"},
        {L"连接谱面", L"运行 MuseSync Bridge，确认开启提示。速度、拍号随谱面变化；切换谱面后刷新。\n关闭「跟随谱面」可手动设置，顶部拍号块可换拍。"},
        {L"对齐第一拍", L"Ctrl / Alt + 拖动网格，对齐第 1 小节与音乐第一拍；加按 Shift 微调。\n起始偏移可输入毫秒，回转箭头清零。"},
        {L"移调与音量", L"半音 ±24，音分 ±100，可叠加且不改变时长。\n回转箭头恢复原音，滑条调整音量。"},
        {L"浏览与定位", L"拖动 / 滚轮平移；Ctrl / Alt + 滚轮缩放；双击全览。点击波形或拖动进度条跳转播放。\n顶部三角为音频，底部方块为谱面；错位时回到播放头，仍异常则重启宿主。"},
        {L"悬浮与主题", L"主界面与悬浮条均显示「小节-拍数」，如 73-3。\n收起后继续同步；拖动悬浮条移动，点展开或双击还原。调色盘切换主题，随工程保存。"},
    };
    RECT client {};
    ::GetClientRect (m_hwnd, &client);
    ::FillRect (dc, &client, m_bgBrush);
    const auto l = ui::arrangeGuide (m_layout.width, m_layout.height);
    auto rect = [this] (ui::Rect r) { return RECT {px (r.x), px (r.y), px (r.right ()), px (r.bottom ())}; };
    auto text = [&] (const wchar_t* value, ui::Rect box, HFONT font, COLORREF color, UINT flags) {
        RECT r = rect (box);
        ::SelectObject (dc, font);
        ::SetTextColor (dc, color);
        ::DrawTextW (dc, value, -1, &r, flags | DT_NOPREFIX);
    };
    ::SetBkMode (dc, TRANSPARENT);
    text (L"使用指南", l.title, m_fontBold, kInk, DT_SINGLELINE | DT_VCENTER);
    DRAWITEMSTRUCT close {};
    close.CtlType = ODT_BUTTON;
    close.CtlID = IDC_HELP_CLOSE;
    close.hDC = dc;
    close.rcItem = rect (l.close);
    drawItem (&close);
    HBRUSH rule = ::CreateSolidBrush (kSoftRule);
    for (const int y : {62, l.about.y - 12})
    {
        RECT r {px (24), px (y), client.right - px (24), px (y + 1)};
        ::FillRect (dc, &r, rule);
    }
    ::DeleteObject (rule);
    for (int i = 0; i < 6; ++i)
    {
        const auto s = l.sections[i];
        wchar_t number[8] {};
        std::swprintf (number, 8, L"%02d", i + 1);
        text (number, {s.x, s.y, 28, 22}, m_fontMono, kAccent, DT_SINGLELINE | DT_VCENTER);
        text (guide[i].title, {s.x + 34, s.y, s.w - 34, 22}, m_fontHeading, kInk, DT_SINGLELINE | DT_VCENTER);
        text (guide[i].body, {s.x, s.y + 28, s.w, s.h - 28}, m_fontSmall, kMuted,
              DT_WORDBREAK | DT_EDITCONTROL);
    }
    text (utf8ToWide (std::string ("MuseSync  ") + MUSETONE_VERSION).c_str (), l.about,
          m_fontHeading, kInk, DT_SINGLELINE);
    text (L"关于  ·  @爱鸽咕子", l.credit, m_fontSmall, kMuted, DT_SINGLELINE);
    std::wstring message = m_updateStatus.message.empty () ? L"更新" : m_updateStatus.message;
    if (m_updateStatus.phase == updates::Phase::downloading)
    {
        wchar_t amount[96] {};
        if (m_updateStatus.total)
            std::swprintf (amount, 96, L"正在下载  %.1f / %.1f MB", m_updateStatus.received / 1048576.0, m_updateStatus.total / 1048576.0);
        else std::swprintf (amount, 96, L"正在下载  %.1f MB", m_updateStatus.received / 1048576.0);
        message = amount;
    }
    text (message.c_str (), l.updateStatus, m_fontSmall, kMuted, DT_SINGLELINE | DT_END_ELLIPSIS);
    RECT track = rect (l.updateProgress);
    HBRUSH bg = ::CreateSolidBrush (kSoftRule);
    ::FillRect (dc, &track, bg);
    ::DeleteObject (bg);
    if (m_updateStatus.phase == updates::Phase::ready || m_updateStatus.total)
    {
        const double ratio = m_updateStatus.phase == updates::Phase::ready ? 1.0 :
            std::min (1.0, double (m_updateStatus.received) / m_updateStatus.total);
        track.right = track.left + static_cast<int> ((track.right - track.left) * ratio);
        HBRUSH fill = ::CreateSolidBrush (kAccent);
        ::FillRect (dc, &track, fill);
        ::DeleteObject (fill);
    }
}

void WinView::refreshUpdates ()
{
    if (!m_autoUpdateChecked && !::GetEnvironmentVariableW (L"MUSESYNC_DISABLE_UPDATE_CHECK", nullptr, 0))
    {
        m_autoUpdateChecked = true;
        m_updater.check ();
    }
    const auto status = m_updater.status ();
    if (status.revision == m_updateStatus.revision) return;
    m_updateStatus = status;
    const bool busy = status.phase == updates::Phase::checking || status.phase == updates::Phase::downloading;
    const bool newer = !status.release.filename.empty () && updates::newerVersion (status.release.version, MUSETONE_VERSION);
    ::EnableWindow (m_updateCheck, !busy);
    ::EnableWindow (m_updateAction, status.phase == updates::Phase::available ||
        status.phase == updates::Phase::ready || status.phase == updates::Phase::downloading ||
        (status.phase == updates::Phase::failed && newer));
    ::SetWindowTextW (m_updateAction, status.phase == updates::Phase::downloading ? L"取消下载" :
        status.phase == updates::Phase::ready ? L"安装更新" : L"下载更新");
    ::InvalidateRect (m_helpBtn, nullptr, FALSE);
    if (m_helpVisible) ::InvalidateRect (m_hwnd, nullptr, FALSE);
}

void WinView::updateAction ()
{
    const auto s = m_updater.status ();
    if (s.phase == updates::Phase::downloading) { m_updater.cancel (); return; }
    if (s.phase != updates::Phase::ready) { m_updater.download (); refreshUpdates (); return; }
    if (::MessageBoxW (m_hwnd,
        L"安装器已下载并通过校验。\n\n请先保存谱面，关闭 MuseScore 及其他使用该插件的软件，再继续安装。无需卸载旧版。\n\n现在打开安装器？",
        L"MuseSync 更新", MB_OKCANCEL | MB_ICONINFORMATION) != IDOK) return;
    try
    {
        // Recheck immediately before launch in case the saved download was modified.
        if (updates::sha256File (s.installer) != s.release.sha256) throw std::runtime_error ("checksum");
        if (reinterpret_cast<INT_PTR> (::ShellExecuteW (m_hwnd, L"open", s.installer.c_str (), nullptr,
            s.installer.parent_path ().c_str (), SW_SHOWNORMAL)) <= 32) throw std::runtime_error ("launch");
    }
    catch (...) { ::MessageBoxW (m_hwnd, L"无法打开安装器或文件校验失败，请重新检查并下载。", L"MuseSync 更新", MB_OK | MB_ICONERROR); }
}

//------------------------------------------------------------------------------
// 拍号条 与 拍号设置弹层
//
// 拍号条：波形区顶部一条窄带，每个小节一个「N/M」方块，拍号变化点用橙色高亮。
// 点方块 → 弹出该小节的拍号设置。
//
// ⚠️ 弹层与使用指南同一套做法（隐藏所有控件 + 主窗口整块自绘），理由见 setHelp：
//    宿主里的 z 序不可依赖（铁律 15），让层级干脆不参与就不会错。
//------------------------------------------------------------------------------
void WinView::rebuildBars (std::vector<ap::BarMark>& out) const
{
    out.clear ();
    if (!m_backend || !(m_viewSpan > 0.0))
        return;

    const auto grid = ap::resolveGridMetadata (*m_backend);

    // 逐小节推进（拍号变化处每小节长度不同）—— 算法在 gui.h，三端共用同一份
    ap::buildBars (m_viewStart, m_viewStart + m_viewSpan,
                   static_cast<double> (m_backend->offsetSec ()),
                   60.0 / static_cast<double> (grid.bpm),
                   grid.beats, grid.denominator,
                   grid.changes, out, grid.measures, grid.tempos);
}

std::vector<TsStripBox> WinView::tsStripBoxes () const
{
    std::vector<TsStripBox> boxes;

    std::vector<ap::BarMark> bars;
    rebuildBars (bars);
    if (bars.empty ())
        return boxes;

    const int boxTop   = px (3);
    const int boxBot   = boxTop + px (kTsBoxH);
    const int w        = m_tsBoxW > 0 ? m_tsBoxW : px (34);
    const double view1 = m_viewStart + m_viewSpan;
    int lastRight = -1000000;

    for (const ap::BarMark& m : bars)
    {
        if (m.startSec < m_viewStart || m.startSec > view1)
            continue;
        const int x = secToX (m.startSec);

        TsStripBox b;
        const int left = std::clamp (x - w / 2, 0, std::max (0, m_waveW - w));
        b.rc      = { left, boxTop, std::min (m_waveW, left + w), boxBot };
        b.bar     = m.bar;
        b.beats   = m.beats;
        b.denom   = m.denom;
        b.changed = m.changed;

        if (b.rc.left < lastRight + px (kTsBoxGap))
            continue;                       // 挤不下 → 跳过（与 mac 端同一规则）
        if (b.rc.left > m_waveW)
            break;
        lastRight = b.rc.right;
        boxes.push_back (b);
    }
    return boxes;
}

void WinView::drawTimeSigStrip (HDC dc, const std::vector<TsStripBox>& boxes)
{
    const int stripH = px (kTsStripH);

    RECT band = { 0, 0, m_waveW, stripH };
    HBRUSH bg = ::CreateSolidBrush (kSurface);
    ::FillRect (dc, &band, bg);
    ::DeleteObject (bg);

    RECT sep = { 0, stripH - std::max (1, px (1)), m_waveW, stripH };
    HBRUSH sp = ::CreateSolidBrush (kRule);
    ::FillRect (dc, &sep, sp);
    ::DeleteObject (sp);

    ::SetBkMode (dc, TRANSPARENT);
    ::SelectObject (dc, m_fontSmall);

    for (const TsStripBox& b : boxes)
    {
        HBRUSH br = ::CreateSolidBrush (b.changed ? kAccent : kSoftRule);
        ::FillRect (dc, &b.rc, br);
        ::DeleteObject (br);

        ::SetTextColor (dc, b.changed ? RGB (255, 250, 245) : kMuted);
        wchar_t label[32];
        std::swprintf (label, 32, L"%d/%d", b.beats, b.denom);
        RECT tr = b.rc;
        ::DrawTextW (dc, label, -1, &tr,
                     DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }
}

bool WinView::waveTimeSigBoxAt (int x, int y, int& bar) const
{
    if (!m_backend || !m_backend->hasAudio ())
        return false;
    if (y >= px (kTsStripH))
        return false;                       // 落在波形区，不是拍号条

    const std::vector<TsStripBox> boxes = tsStripBoxes ();
    for (const TsStripBox& b : boxes)
    {
        RECT hit = b.rc;
        ::InflateRect (&hit, px (3), px (3));
        if (x >= hit.left && x < hit.right && y >= hit.top && y < hit.bottom)
        {
            bar = b.bar;
            return true;
        }
    }
    return false;
}

//---- 弹层 ----------------------------------------------------------------
void WinView::openTimeSigPopup (int bar)
{
    if (m_backend && m_backend->followScore ()) return;
    if (!m_backend || !m_backend->hasAudio ())
        return;

    // 该小节【实际生效】的拍号 = 「起小节 <= bar」的最后一条变化；没有就用全局默认
    int  beats = m_backend->gridBeatsPerBar ();
    int  denom = m_backend->gridBeatDenominator ();
    if (beats < 1) beats = 4;
    if (denom < 1) denom = 4;
    bool has = false;
    std::vector<ap::TimeSigChange> changes;
    m_backend->getTimeSigChanges (changes);
    for (const ap::TimeSigChange& c : changes)
    {
        if (c.bar == bar) has = true;
        if (c.bar <= bar) { beats = c.beats; denom = c.denom; }
    }

    setHelp (false);                        // 别和指南叠在一起
    m_tsPopupVisible   = true;
    m_tsPopupBar       = bar;
    m_tsPopupBeats     = beats;
    m_tsPopupDenom     = denom;
    m_tsPopupHasChange = has;

    showControls (false);                   // 层级不参与：藏掉所有控件，由主窗口整块自绘
    ::SetFocus (m_hwnd);
    ::InvalidateRect (m_hwnd, nullptr, TRUE);
}

void WinView::closeTimeSigPopup ()
{
    if (!m_tsPopupVisible)
        return;
    m_tsPopupVisible = false;
    m_tsPopupBtns.clear ();
    showControls (true);
    if (m_wave)
        ::InvalidateRect (m_wave, nullptr, TRUE);   // 恢复波形区（含拍号条）
    ::InvalidateRect (m_hwnd, nullptr, TRUE);
}

void WinView::applyTimeSigChange (int bar, int beats, int denom)
{
    if (!m_backend)
        return;

    // 「设成跟继承值一样」= 这一小节其实没有变化 → 直接取消变化点，
    // 免得留下一个橙色方块却毫无几何作用。
    int inhB = m_backend->gridBeatsPerBar ();
    int inhD = m_backend->gridBeatDenominator ();
    if (inhB < 1) inhB = 4;
    if (inhD < 1) inhD = 4;
    std::vector<ap::TimeSigChange> changes;
    m_backend->getTimeSigChanges (changes);
    for (const ap::TimeSigChange& c : changes)
        if (c.bar < bar) { inhB = c.beats; inhD = c.denom; }

    const bool same = (beats == inhB && denom == inhD);
    if (same)
        m_backend->clearTimeSigChange (bar);
    else
        m_backend->setTimeSigChange (bar, beats, denom);

    m_tsPopupBeats     = beats;
    m_tsPopupDenom     = denom;
    m_tsPopupHasChange = !same;

    if (m_wave)
        ::InvalidateRect (m_wave, nullptr, FALSE);
    ::InvalidateRect (m_hwnd, nullptr, TRUE);
}

void WinView::removeTimeSigChange (int bar)
{
    if (!m_backend)
        return;
    m_backend->clearTimeSigChange (bar);

    int beats = m_backend->gridBeatsPerBar ();
    int denom = m_backend->gridBeatDenominator ();
    if (beats < 1) beats = 4;
    if (denom < 1) denom = 4;
    std::vector<ap::TimeSigChange> changes;
    m_backend->getTimeSigChanges (changes);
    for (const ap::TimeSigChange& c : changes)
        if (c.bar <= bar) { beats = c.beats; denom = c.denom; }

    m_tsPopupBeats     = beats;
    m_tsPopupDenom     = denom;
    m_tsPopupHasChange = false;

    if (m_wave)
        ::InvalidateRect (m_wave, nullptr, FALSE);
    ::InvalidateRect (m_hwnd, nullptr, TRUE);
}

void WinView::layoutTsPopup ()
{
    m_tsPopupBtns.clear ();

    RECT client = {};
    ::GetClientRect (m_hwnd, &client);

    const int padX = px (14), btnW = px (34), btnH = px (22), gap = px (5);
    const int labelW = px (34), headH = px (30), rowH = px (30), removeH = px (22);

    const int row1W = labelW + px (6) + kTsBeatChoiceCount  * (btnW + gap) - gap;
    const int row2W = labelW + px (6) + kTsDenomChoiceCount * (btnW + gap) - gap;
    const int boxW  = std::max (row1W, row2W) + padX * 2;
    const int boxH  = headH + rowH * 2 + (m_tsPopupHasChange ? px (26) : 0) + px (14);

    // ⚠️ RECT 的成员是 LONG，不是 int —— 与 int 混用会让 std::max 推导不出模板
    //    参数（MSVC 报 C2672「no matching overloaded function found」）。显式转回 int。
    const int boxX = std::max (0, static_cast<int> ((client.right  - boxW) / 2));
    const int boxY = std::max (0, static_cast<int> ((client.bottom - boxH) / 2));
    m_tsPopupBox = { boxX, boxY, boxX + boxW, boxY + boxH };

    const int startX = static_cast<int> (m_tsPopupBox.left) + padX + labelW + px (6);
    int y = static_cast<int> (m_tsPopupBox.top) + headH;
    int x = startX;
    for (int i = 0; i < kTsBeatChoiceCount; ++i)
    {
        TsPopupBtn b {};
        b.rc    = { x, y, x + btnW, y + btnH };
        b.kind  = 0;
        b.value = kTsBeatChoices[i];
        m_tsPopupBtns.push_back (b);
        x += btnW + gap;
    }
    y += rowH;
    x = startX;
    for (int i = 0; i < kTsDenomChoiceCount; ++i)
    {
        TsPopupBtn b {};
        b.rc    = { x, y, x + btnW, y + btnH };
        b.kind  = 1;
        b.value = kTsDenomChoices[i];
        m_tsPopupBtns.push_back (b);
        x += btnW + gap;
    }
    if (m_tsPopupHasChange)
    {
        y += rowH;
        TsPopupBtn b {};
        b.rc    = { m_tsPopupBox.left + padX, y, m_tsPopupBox.right - padX, y + removeH };
        b.kind  = 2;
        b.value = 0;
        m_tsPopupBtns.push_back (b);
    }
}

void WinView::paintTimeSigPopup (HDC dc)
{
    RECT client = {};
    ::GetClientRect (m_hwnd, &client);
    layoutTsPopup ();

    // ⚠️ GDI 的 FillRect 没有 alpha 通道。要半透明遮罩就得引 msimg32/AlphaBlend，
    //    本端没有实机验证条件（CI 只编不跑），不值得为一个纯观感引入新依赖 ——
    //    改用不透明底色：这一层本来就是模态的，控件已被 showControls(false) 藏起来。
    HBRUSH backdrop = ::CreateSolidBrush (kPaper);
    ::FillRect (dc, &client, backdrop);
    ::DeleteObject (backdrop);

    HBRUSH boxBg = ::CreateSolidBrush (kSurface);
    ::FillRect (dc, &m_tsPopupBox, boxBg);
    ::DeleteObject (boxBg);
    HBRUSH border = ::CreateSolidBrush (kRule);
    ::FrameRect (dc, &m_tsPopupBox, border);
    ::DeleteObject (border);

    ::SetBkMode (dc, TRANSPARENT);

    ::SelectObject (dc, m_font);
    ::SetTextColor (dc, kAccentDark);
    wchar_t title[64];
    std::swprintf (title, 64, L"第 %d 小节  拍号", m_tsPopupBar);
    ::TextOutW (dc, m_tsPopupBox.left + px (14), m_tsPopupBox.top + px (8), title,
                static_cast<int> (std::wcslen (title)));

    ::SelectObject (dc, m_fontSmall);
    ::SetTextColor (dc, kMuted);
    ::TextOutW (dc, m_tsPopupBox.left + px (14), m_tsPopupBox.top + px (34), L"分子", 2);
    ::TextOutW (dc, m_tsPopupBox.left + px (14), m_tsPopupBox.top + px (64), L"分母", 2);

    for (const TsPopupBtn& b : m_tsPopupBtns)
    {
        const bool sel = (b.kind == 0 && b.value == m_tsPopupBeats)
                      || (b.kind == 1 && b.value == m_tsPopupDenom);
        COLORREF fill, text;
        if (b.kind == 2)      { fill = kAccentSoft; text = kAccentDark; }
        else if (sel)         { fill = kAccent; text = RGB (255, 250, 245); }
        else                  { fill = kSurface;   text = kInk; }

        HBRUSH br = ::CreateSolidBrush (fill);
        ::FillRect (dc, &b.rc, br);
        ::DeleteObject (br);

        ::SelectObject (dc, m_fontSmall);
        ::SetTextColor (dc, text);
        RECT tr = b.rc;
        if (b.kind == 2)
        {
            ::DrawTextW (dc, L"取消该小节的变化", -1, &tr,
                         DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        }
        else
        {
            wchar_t label[32];
            std::swprintf (label, 32, L"%d", b.value);
            ::DrawTextW (dc, label, -1, &tr,
                         DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        }
    }
}

bool WinView::onTimeSigPopupClick (int x, int y)
{
    if (!m_tsPopupVisible)
        return false;

    layoutTsPopup ();
    for (const TsPopupBtn& b : m_tsPopupBtns)
    {
        RECT hit = b.rc;
        ::InflateRect (&hit, px (2), px (2));
        if (x < hit.left || x >= hit.right || y < hit.top || y >= hit.bottom)
            continue;

        if (b.kind == 0)
            applyTimeSigChange (m_tsPopupBar, b.value, m_tsPopupDenom);
        else if (b.kind == 1)
            applyTimeSigChange (m_tsPopupBar, m_tsPopupBeats, b.value);
        else
            removeTimeSigChange (m_tsPopupBar);
        return true;
    }

    closeTimeSigPopup ();       // 点面板外 = 关闭
    return true;
}

//------------------------------------------------------------------------------
void WinView::markUserScrolling ()
{
    m_userScrolling = true;
    m_lastUserScrollTick = ::GetTickCount ();
}

// 用户停手 2 秒后自动恢复跟随。
// ⚠️ 旧版（mac 端曾踩过）一旦置 true 就永久停跟随，用户滚一下之后播放头就再也
//    不跟了；这里必须有超时恢复。
void WinView::recoverAutoFollow ()
{
    if (!m_userScrolling)
        return;
    if (m_dragging)             // 还在拖，别打断
        return;
    if ((::GetTickCount () - m_lastUserScrollTick) >= kUserScrollHoldMs)
        m_userScrolling = false;
}

// 视野跟随播放头。规则（与 macOS 端逐条对齐）：
//   ① 用户手动操作期间 → 完全不干预，播放头允许跑出画面；
//   ② 播放头远在画面之外（超一整个屏）→ 不追（这是「不抢用户视野」的关键一条）；
//   ③ 唯一例外 allowJump：宿主自己把播放头挪了 → 允许跟过去。
void WinView::followPlayheadTo (double audioSec, bool allowJump)
{
    if (!m_followScroll)
        return;
    if (m_dragging || m_helpVisible)
        return;
    if (!(m_viewSpan > 0.0))
        return;

    // ① 用户在看别处 → 不干预（宿主主动跳转时例外）
    if (m_userScrolling && !allowJump)
        return;

    const double v0 = m_viewStart;
    const double v1 = v0 + m_viewSpan;
    const double margin = m_viewSpan * 0.25;   // 播放头离边缘多远开始滚

    // ② 远在画面之外 → 不追。容差取一整个屏：极端放大时播放头两帧之间就能移动
    //    大半屏，容差太小会漏跟。
    if (!allowJump && (audioSec < v0 - m_viewSpan || audioSec > v1 + m_viewSpan))
        return;

    // ③ 播放头落在画面左侧（循环回卷 / 向前跳转）→ 对到 25% 处，让它重新可见
    if (audioSec < v0)
    {
        m_viewStart = audioSec - margin;
        clampView ();
        requestWaveRepaint (false);
        return;
    }

    // ④ 播放头快到右边缘 → 顺滑向右滚
    if (audioSec > v1 - margin)
    {
        m_viewStart = audioSec - margin;
        clampView ();
        requestWaveRepaint (false);
    }
}

void WinView::requestWaveRepaint (bool fromMouse)
{
    if (!m_wave)
        return;

    if (fromMouse)
    {
        const DWORD now = ::GetTickCount ();
        if ((now - m_lastMousePaintTick) < kDragPaintGapMs)
        {
            // 节流掉：记住「欠一帧」，交给 50ms 的界面定时器补上。
            m_mousePaintPending = true;
            return;
        }
        m_lastMousePaintTick = now;
    }
    m_mousePaintPending = false;
    m_lastPaintKey = 1.0e30;      // 强制下一帧真的重画（指纹比对放行）
    ::InvalidateRect (m_wave, nullptr, FALSE);
}

/// 光标（【屏幕】坐标，滚轮的 lParam 就是屏幕坐标）是否落在波形区上。
/// 滚轮消息在 Windows 上是发给【焦点窗口】的，不是发给光标下的窗口
/// （除非开了「悬停滚动非活动窗口」），所以必须在主窗口里自己按坐标判断 ——
/// 这正是用户报的「滚轮不平移」的根因：消息压根没送到波形窗口上。
bool WinView::waveHitTest (int screenX, int screenY) const
{
    if (!m_wave)
        return false;
    RECT r = {};
    if (!::GetWindowRect (m_wave, &r))
        return false;
    return (screenX >= r.left && screenX < r.right &&
            screenY >= r.top  && screenY < r.bottom);
}

void WinView::onMouseDown (int x, int y, int mods)
{
    if (!m_backend || !m_backend->hasAudio ())
        return;

    // 顶部拍号条：点中方块 → 弹出该小节的拍号设置。
    // ⚠️ 命中与没命中都要把这次点击吃掉 —— 否则会掉进下面的拖动分支，
    //    点一下拍号条就把视野挪走了（用户会以为是 bug）。
    if (y < px (kTsStripH))
    {
        int bar = 0;
        if (waveTimeSigBoxAt (x, y, bar))
            openTimeSigPopup (bar);
        return;
    }

    // ⭐ 让波形区拿到键盘焦点：滚轮消息是发给焦点窗口的，不点一下它，
    //    滚轮只会落到「上一次点过的控件」上（比如 BPM 输入框）。
    ::SetFocus (m_wave);

    ::SetCapture (m_wave);
    m_dragging = true;
    m_dragStartX = x;
    m_dragStartY = y;
    m_dragMoved = false;
    m_dragOffset = ((mods & MK_CONTROL) != 0) || ((mods & AP_MK_ALT) != 0);
    // 改偏移时记 offset，平移视野时记 viewStart —— 两者互斥，复用同一个字段。
    m_dragStartVal = m_dragOffset ? m_backend->offsetSec () : m_viewStart;

    // 平移视野 = 用户在主动看别处 → 暂停自动跟随 2 秒（拖完自然恢复）。
    // 改偏移【不算】：那只动网格锚点，播放头本身没被挪走。
    if (!m_dragOffset)
        markUserScrolling ();

    refreshLabels ();
    requestWaveRepaint (false);
}

void WinView::onMouseMove (int x, int y, int mods)
{
    (void) y;
    if (!m_dragging || !m_backend)
        return;

    if (std::abs (x - m_dragStartX) >= px (4) || std::abs (y - m_dragStartY) >= px (4))
        m_dragMoved = true;
    if (!m_dragOffset && !m_dragMoved) return;

    if (m_dragOffset)
    {
        const double dxSec = (x - m_dragStartX) / static_cast<double> (m_waveW) * m_viewSpan;
        double v = m_dragStartVal + dxSec;
        if (mods & MK_SHIFT)
            v = m_dragStartVal + dxSec * 0.1;
        if (v > 3600.0)  v = 3600.0;
        if (v < -3600.0) v = -3600.0;
        m_backend->setOffsetSec (static_cast<float> (v));
        // 只有偏移会改变标签内容；平移时时间/偏移都没变，不必每帧刷六个 STATIC
        // （SetWindowTextW 会连带重绘控件，鼠标消息密集时是实打实的开销）。
        refreshLabels ();
    }
    else if (m_waveW > 0 && m_viewSpan > 0.0)
    {
        // 平移视野（抓手）：把内容往右拉 → 视野往左移，看到更早的音频。
        const double dxSec = (x - m_dragStartX) / static_cast<double> (m_waveW) * m_viewSpan;
        m_viewStart = m_dragStartVal - dxSec;
        clampView ();
        markUserScrolling ();
    }

    requestWaveRepaint (true);
}

void WinView::onMouseUp (int x, int y, bool cancel)
{
    if (m_dragging)
    {
        const bool click = !cancel && !m_dragMoved && !m_dragOffset &&
            std::abs (x - m_dragStartX) < px (4) && std::abs (y - m_dragStartY) < px (4) &&
            x >= 0 && x < m_waveW && y >= px (kTsStripH) && y < m_waveH;
        m_dragging = false;
        m_lastUserScrollTick = ::GetTickCount ();   // 松手后再等 2 秒才恢复跟随
        ::ReleaseCapture ();
        if (click && m_backend && m_waveW > 0)
        {
            const double audioSec = m_viewStart + x * m_viewSpan / m_waveW;
            seekFromAudio (audioSec);
        }
    }
}

void WinView::onWheelDelta (int delta, int mods, bool horizontal)
{
    if (!m_backend || !m_backend->hasAudio ())
        return;

    if (mods & (MK_CONTROL | AP_MK_ALT))
    {
        zoomBy (delta > 0 ? 0.8 : 1.25, m_backend->positionSec ());
    }
    else
    {
        markUserScrolling ();
        const double step = (delta / 120.0) * m_viewSpan * 0.1;
        // 纵向滚轮：向上 = 往更早看（与旧版一致）。
        // 横向滚轮（WM_MOUSEHWHEEL，delta > 0 = 向右拨）：往更晚看。
        m_viewStart += horizontal ? step : -step;
        clampView ();
    }
    refreshLabels ();
    requestWaveRepaint (false);
}

void WinView::onDoubleClick ()
{
    zoomFit ();
    refreshLabels ();
    requestWaveRepaint (false);
}

void WinView::zoomBy (double factor, double centerSec)
{
    // ⭐ 缩放也算「用户在看别处」：否则点「放大/缩小」按钮（它们不经过滚轮）
    //    时播放头会被挤出画面，紧接着被自动跟随拽回去 → 画面抽搐。
    //    放在这里就不漏任何缩放入口。
    markUserScrolling ();

    const double oldSpan = m_viewSpan;
    m_viewSpan *= factor;
    if (m_viewSpan < 0.05)   m_viewSpan = 0.05;
    if (m_viewSpan > 3600.0) m_viewSpan = 3600.0;
    m_viewStart = centerSec - (centerSec - m_viewStart) * (m_viewSpan / oldSpan);
    clampView ();
}

void WinView::zoomFit ()
{
    if (!m_backend)
        return;
    markUserScrolling ();
    const double dur = m_backend->durationSec ();
    m_viewStart = 0.0;
    m_viewSpan = dur > 0.0 ? dur : 8.0;
}

void WinView::clampView ()
{
    const double dur = m_backend ? m_backend->durationSec () : 0.0;
    if (m_viewStart < 0.0)
        m_viewStart = 0.0;
    if (dur > 0.0 && m_viewStart > dur)
        m_viewStart = dur;
}

// 把视野挪到 sec 处 —— 播放头落在画面 25% 的位置（与 macOS 端同一套规则）。
// 「回到播放头」除了重新对齐，还要把画面也带回去：只对齐不挪视野，用户会看到
// 「显示已对齐，但画面里什么都没有」，比不对齐还困惑。
void WinView::centerViewOn (double sec)
{
    const double dur = m_backend ? m_backend->durationSec () : 0.0;
    if (m_viewSpan <= 0.0)
        return;
    if (dur > 0.0 && m_viewSpan >= dur)
        return;                 // 全览：整段都在画面里，无需滚动
    m_viewStart = sec - m_viewSpan * 0.25;
    clampView ();
}

void WinView::onTimer ()
{
    if (m_destroying) return;
    refreshUpdates ();
    // 宿主容器窗口若装不下我们（老版本可能给了个 340 宽的窗口），先缩回去，
    // 否则右侧那排按钮会被父窗口裁掉 —— 那比界面窄更糟。
    syncSizeToParent ();

    if (!m_backend)
        return;

    if (m_theme != m_backend->theme ()) applyTheme (m_backend->theme ());
    const DWORD tick = ::GetTickCount ();
    if (tick - m_lastScoreCheck >= 1000)
    {
        m_lastScoreCheck = tick;
        syncCurrentScore (false);
    }

    const PlugView::Backend::HostTimeline tl = m_backend->hostTimeline ();
    syncVideoToTimeline (false);
    const double audioNow = m_backend->positionSec ();
    const double durNow   = m_backend->durationSec ();
    const double offNow   = static_cast<double> (m_backend->offsetSec ());
    const double diffNow  = audioNow - (tl.playheadSec + offNow);

    // ---- 「点完『回到播放头』仍然错位」= 跟随本身失效 ----
    // 正常点一下就对齐了。若 3 秒内又错位，说明有某个跟随条件持续为假 ——
    // 徽标文案升级为「建议重启插件」，并记一条日志。
    // 判据与波形区徽标完全一致（同样排除起播追赶期与音频尾端）。
    {
        const bool diverging =
            tl.playing && tl.playheadValid && durNow > 0.60
            && audioNow > 0.30 && audioNow < durNow - 0.30
            && std::fabs (diffNow) > kDivergenceWarnSec;

        const DWORD nowT = ::GetTickCount ();
        if (diverging && (nowT - m_lastBackTick) < kFollowFailWindowMs)
        {
            m_followFail = true;
            if ((nowT - m_followFailLoggedTick) > kFollowFailLogGapMs)
            {
                m_followFailLoggedTick = nowT;
                ap::crashLog ("跟随可能已失效：点「回到播放头」后 %.1f 秒仍错位 %.2f 秒"
                              "（音频 %.3f / 谱面 %.3f + 偏移 %.3f）",
                              static_cast<double> (nowT - m_lastBackTick) * 0.001,
                              std::fabs (diffNow), audioNow, tl.playheadSec, offNow);
            }
        }
        else
        {
            m_followFail = false;
        }
    }

    // ---- 视野跟随 ----
    // ⭐ 先区分「播放头自己在走」和「宿主把它挪走了」：前者画面只许顺滑滚，
    //    后者画面该跟过去。20Hz 采样下正常前进每帧只有几十毫秒；
    //    超过 kHostJumpSec 就是宿主跳变（点小节 / 循环回卷 / 拖播放头）。
    bool hostJumped = false;
    if (tl.playheadValid)
    {
        if (m_hasPrevPlayhead &&
            std::fabs (tl.playheadSec - m_prevPlayheadSec) > kHostJumpSec)
            hostJumped = true;
        m_prevPlayheadSec = tl.playheadSec;
        m_hasPrevPlayhead = true;
    }

    recoverAutoFollow ();

    // ⭐ 宿主自己跳的（点小节 / 循环回卷 / 拖播放头）就算停在【暂停】状态也要把
    //    画面带过去 —— 扒谱就是在暂停下一个小节一个小节点的。以前这里写死了
    //    `tl.playing`，于是暂停时换位置画面纹丝不动，「空白乐谱直接定位到音乐」
    //    这件事根本做不到。自然播放推进（allowJump=false）时仍然只许顺滑滚。
    if (tl.playheadValid && (tl.playing || hostJumped))
        followPlayheadTo (tl.playheadSec + offNow, hostJumped);

    refreshLabels ();

    if (m_collapsed)
    {
        const std::wstring status = m_musicalText + L"  " + m_transportText + L"  " + m_timeText;
        if (m_floatingText != status)
        {
            m_floatingText = status;
            ::InvalidateRect (m_floating, nullptr, FALSE);
        }
    }

    // ---- 波形区重绘：只在内容真的变了的时候 ----
    // 旧版每 50ms 无条件重画整块波形（连暂停、连没有音频时都在画）。
    // 在虚拟机上这是纯白烧 CPU，会跟音频线程抢核 → 出爆音/变速。
    // 这里用一个「内容指纹」比对：位置 / 谱面位置 / 偏移 / 视野 / 播放态 / 徽标态
    // 只要有一项变了才重画；被节流掉的鼠标重绘也在这里补上。
    const double key = audioNow * 1.0 + tl.playheadSec * 7.0 + offNow * 11.0
                     + m_viewStart * 13.0 + m_viewSpan * 17.0
                     + (tl.playing ? 3.0 : 5.0) + (m_followFail ? 19.0 : 23.0)
                     + (tl.playheadValid ? 29.0 : 31.0)
                     + tl.bpm * 37.0 + tl.scoreBpm * 41.0
                     + tl.beatsPerBar * 43.0 + tl.beatDenominator * 47.0
                     + tl.scoreBeats * 53.0 + tl.scoreDenominator * 59.0;
    if (!m_collapsed && !m_videoView && (m_mousePaintPending || key != m_lastPaintKey))
    {
        m_mousePaintPending = false;
        m_lastPaintKey = key;
        ::InvalidateRect (m_wave, nullptr, FALSE);
    }

    // ---- 视野状态回存：关掉编辑器再打开要回到同一段视野 ----
    // ⭐ 集中在这里比对是刻意的：视野的改动入口很多（滚轮 / 拖动 / 缩放 / 全览 /
    //    回到播放头 / 自动跟随），逐个入口去写必然漏一个（见 MEMORY 铁律 9）。
    if (m_backend->hasAudio () && m_viewSpan > 0.0
        && (std::fabs (m_viewStart - m_pushedViewStart) > 1e-6
         || std::fabs (m_viewSpan - m_pushedViewSpan) > 1e-6))
    {
        m_pushedViewStart = m_viewStart;
        m_pushedViewSpan  = m_viewSpan;
        m_backend->setViewState (m_viewStart, m_viewSpan);
    }
}

void WinView::syncVideoToTimeline (bool force)
{
    if (!m_videoView || !m_backend) return;
    const auto timeline = m_backend->hostTimeline ();
    const double offset = static_cast<double> (m_backend->offsetSec ());
    double selectionSeconds = 0.0;
    const bool selectionValid = ap::readScoreSelection (
        m_scoreSession, ap::scoreBridgeNowMs (), selectionSeconds);
    m_videoScoreSec = m_videoTimeline.update (timeline.playheadSec, timeline.playheadValid,
        timeline.playing, selectionValid, selectionSeconds);
    const double videoSeconds = m_videoScoreSec + offset;
    const DWORD now = ::GetTickCount ();
    const bool initialized = m_videoSyncInitialized;
    const bool offsetChanged = initialized && std::fabs (offset - m_lastVideoOffsetSec) > 1e-4;
    const double elapsed = initialized ?
        std::max (0.0, static_cast<double> (now - m_lastVideoSyncTick) * 0.001) : 0.0;
    const double delta = initialized ? videoSeconds - m_lastVideoSyncSec : 0.0;
    // A fixed delta threshold mistakes sparse host updates or faster playback
    // for seeks, which resets the video's tempo estimate to 1x. Scale the
    // forward allowance to elapsed wall time; backward motion is always a seek.
    const double expectedAdvance = std::min (elapsed, 1.5) * 4.0 + 0.15;
    const bool timelineJump = initialized &&
        (timeline.playing && m_lastVideoPlaying ?
            (delta < -0.05 || delta > expectedAdvance) : std::fabs (delta) > 0.01);
    const bool jumped = !initialized || offsetChanged || timelineJump;
    const bool stateChanged = !initialized || timeline.playing != m_lastVideoPlaying;
    if (!force && !jumped && !stateChanged && now - m_lastVideoSyncTick < 100) return;

    m_videoView->sync (videoSeconds, timeline.playheadValid && timeline.playing,
                       force || jumped);
    m_lastVideoSyncTick = now;
    m_lastVideoSyncSec = videoSeconds;
    m_lastVideoOffsetSec = offset;
    m_lastVideoPlaying = timeline.playing;
    m_videoSyncInitialized = true;
}

void WinView::refreshLabels ()
{
    if (!m_backend)
        return;

    auto update = [this] (std::wstring& current, const std::wstring& next, ui::Rect box) {
        if (current == next) return;
        current = next;
        if (m_collapsed && m_floating) ::InvalidateRect (m_floating, nullptr, FALSE);
        RECT r = {px (box.x), px (box.y), px (box.right ()), px (box.bottom ())};
        ::InvalidateRect (m_hwnd, &r, FALSE);
    };
    char buf[512];

    const double duration = mediaDuration ();
    const bool hasProgress = (m_videoView || m_backend->hasAudio ()) && duration > 0;
    if (!hasProgress && m_progressDragging)
    {
        m_progressDragging = false;
        if (::GetCapture () == m_progressSlider) ::ReleaseCapture ();
    }
    if (m_progressSlider)
    {
        if (!!::IsWindowEnabled (m_progressSlider) != hasProgress)
            ::EnableWindow (m_progressSlider, hasProgress);
        if (!m_progressDragging)
        {
            const int progress = hasProgress ? static_cast<int> (std::lround (
                std::clamp (mediaPosition () / duration, 0.0, 1.0) * 10000)) : 0;
            if (::SendMessageW (m_progressSlider, TBM_GETPOS, 0, 0) != progress)
            {
                ::SendMessageW (m_progressSlider, TBM_SETPOS, FALSE, progress);
                ::InvalidateRect (m_progressSlider, nullptr, FALSE);
            }
        }
    }
    const double position = m_progressDragging ? duration *
        ::SendMessageW (m_progressSlider, TBM_GETPOS, 0, 0) / 10000.0 : mediaPosition ();
    const std::string pos = formatTime (position);
    const std::string total = formatTime (duration);
    std::snprintf (buf, sizeof buf, "%s / %s", pos.c_str (), total.c_str ());
    update (m_timeText, utf8ToWide (buf), m_layout.time);
    const auto analyzer = m_backend->tempoAnalyzer ();
    const auto tempo = analyzer ? analyzer->snapshot () : nullptr;
    std::wstring detected;
    if (tempo && tempo->state != TempoResult::State::Empty)
    {
        const double bpm = tempo->bpmAt (position);
        if (bpm > 0)
        {
            wchar_t value[64] {};
            std::swprintf (value, 64, L"素材 ≈ %.1f BPM", bpm);
            detected = value;
        }
        else detected = L"素材 BPM · 未识别";
        if (tempo->state == TempoResult::State::Analyzing)
            detected += L" · " + std::to_wstring (static_cast<int> (tempo->progress * 100)) + L"%";
        else if (tempo->state == TempoResult::State::Live && bpm <= 0)
            detected = L"素材 BPM · 等待连续音频";
        else if (tempo->state == TempoResult::State::Error)
            detected = L"素材 BPM · 音轨分析失败";
    }
    update (m_detectedTempoText, detected, m_layout.detectedTempo);
    if (m_tempoResult != tempo || m_backend->hostTimeline ().playing || m_tempoPosition != position)
    {
        const auto r = m_layout.tempoMap;
        RECT area {px (r.x), px (r.y), px (r.right ()), px (r.bottom ())};
        ::InvalidateRect (m_hwnd, &area, FALSE);
    }
    m_tempoResult = tempo;
    m_tempoPosition = position;

    wchar_t numeric[32];
    m_syncingControls = true;
    std::swprintf (numeric, 32, L"%.0f", m_backend->offsetSec () * 1000.0);
    if (m_offsetEdit && ::GetFocus () != m_offsetEdit) setTextIfChanged (m_offsetEdit, numeric);
    const int volume = static_cast<int> (std::lround (m_backend->volume () * 100.0));
    std::swprintf (numeric, 32, L"%d%%", volume);
    update (m_volumeText, numeric, m_layout.volumeValue);
    if (m_volSlider && ::SendMessageW (m_volSlider, TBM_GETPOS, 0, 0) != volume)
        ::SendMessageW (m_volSlider, TBM_SETPOS, TRUE, volume);

    // Do not rewrite a focused edit control while the user is typing.
    if (m_semitoneEdit && ::GetFocus () != m_semitoneEdit)
    {
        wchar_t pitch[32];
        std::swprintf (pitch, 32, L"%d", m_backend->semitones ());
        setTextIfChanged (m_semitoneEdit, pitch);
    }
    if (m_centsEdit && ::GetFocus () != m_centsEdit)
    {
        wchar_t cents[32];
        std::swprintf (cents, 32, L"%d", m_backend->cents ());
        setTextIfChanged (m_centsEdit, cents);
    }
    m_syncingControls = false;
    update (m_pitchText, utf8ToWide (m_backend->pitchStatus ()), m_layout.pitchStatus);

    const PlugView::Backend::HostTimeline tl = m_backend->hostTimeline ();
    const bool follow = m_backend->followScore ();
    const auto grid = ap::resolveGridMetadata (*m_backend);
    const double scorePosition = m_progressDragging ? std::max (0.0, position - m_backend->offsetSec ()) :
        m_videoView ? m_videoScoreSec :
        tl.playheadValid ? tl.playheadSec : std::max (0.0, position - m_backend->offsetSec ());
    const auto musical = ap::musicalPosition (grid, scorePosition);
    std::swprintf (numeric, 32, L"%d-%d", musical.bar, musical.beat);
    update (m_musicalText, numeric, m_layout.musicalPosition);
    double scoreBpm = 0;
    if (tl.scoreTempoValid)
        scoreBpm = ap::TempoTimeline (tl.scoreBpm, tl.scoreTempos).bpmAtSeconds (scorePosition);
    else if (tl.tempoValid)
        scoreBpm = tl.bpm;
    if (scoreBpm > 0 && std::isfinite (scoreBpm))
        std::swprintf (numeric, 32, L"谱面 %.1f", scoreBpm);
    else
        std::swprintf (numeric, 32, L"谱面 --");
    update (m_scoreTempoText, numeric, m_layout.musicalPosition);
    const wchar_t* source = grid.source == ap::GridMetadata::Source::Manual ? L"手动" :
        grid.source == ap::GridMetadata::Source::LiveScore ? L"当前谱面" :
        grid.source == ap::GridMetadata::Source::Host ? L"宿主" :
        grid.source == ap::GridMetadata::Source::LinkedFile ? L"旧版关联文件" : L"默认网格";
    update (m_sourceText, source, m_layout.bpmSource);
    if (m_followControls != static_cast<int> (follow))
    {
        m_followControls = follow;
        ::SendMessageW (m_bpmEdit, EM_SETREADONLY, follow, 0);
        ::EnableWindow (m_bpmUp, !follow);
        ::EnableWindow (m_bpmDown, !follow);
        ::EnableWindow (m_timesig, !follow);
        ::InvalidateRect (m_followScoreCheck, nullptr, FALSE);
    }
    if (follow)
    {
        wchar_t value[32] {};
        std::swprintf (value, 32, L"%.1f", musical.bpm);
        setTextIfChanged (m_bpmEdit, value);
        std::swprintf (value, 32, L"%d/%d", musical.beats, musical.denominator);
        std::wstring meter = value;
        if (m_meterText != meter)
        {
            m_meterText = meter;
            ::InvalidateRect (m_timesig, nullptr, FALSE);
        }
    }

    const std::string path = m_videoView ? wideToUtf8 (m_videoTarget) : m_backend->currentPath ();
    if (m_displayedPath != path || m_fileText.empty ())
    {
        m_displayedPath = path;
        const std::wstring w = utf8ToWide (path);
        const size_t slash = w.find_last_of (L"\\/");
        const std::wstring name = (slash == std::wstring::npos) ? w : w.substr (slash + 1);
        update (m_fileText, name.empty () ? L"未载入素材" : name, m_layout.file);
    }
    ::EnableWindow (m_closeMediaBtn, m_videoView || m_backend->hasAudio ());

    update (m_transportText, !m_videoView && !m_backend->hasAudio () ? L"无媒体" :
            tl.playing ? L"播放中" : L"已暂停", m_layout.transport);
}

void WinView::applyTheme (int theme)
{
    m_theme = ui::clampTheme (theme);
    const auto& t = ui::themes[m_theme];
    auto color = [] (uint32_t rgb) { return RGB ((rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255); };
    kPaper = color (t.paper); kSurface = color (t.surface);
    kInk = color (t.ink); kMuted = color (t.muted);
    kRule = color (t.rule); kSoftRule = color (t.softRule);
    kAccent = color (t.accent); kAccentSoft = color (t.accentSoft);
    kAccentDark = color (t.audioHead); kOlive = color (t.scoreHead);
    if (m_bgBrush) ::DeleteObject (m_bgBrush);
    if (m_inputBrush) ::DeleteObject (m_inputBrush);
    m_bgBrush = ::CreateSolidBrush (kPaper);
    m_inputBrush = ::CreateSolidBrush (kSurface);
    m_lastPaintKey = 1.0e30;
    if (m_hwnd) ::RedrawWindow (m_hwnd, nullptr, nullptr,
                                RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
    if (m_floating) ::RedrawWindow (m_floating, nullptr, nullptr,
                                    RDW_INVALIDATE | RDW_ALLCHILDREN);
}

void WinView::showThemeMenu ()
{
    if (!m_backend) return;
    HMENU menu = ::CreatePopupMenu ();
    std::vector<HBITMAP> swatches;
    HDC screen = ::GetDC (m_hwnd);
    HDC memory = ::CreateCompatibleDC (screen);
    for (int i = 0; i < ui::themeCount; ++i)
    {
        ::AppendMenuW (menu, MF_STRING | (m_theme == i ? MF_CHECKED : 0), 1 + i, ui::themes[i].name);
        HBITMAP swatch = ::CreateCompatibleBitmap (screen, px (14), px (14));
        if (memory && swatch)
        {
            HGDIOBJ previous = ::SelectObject (memory, swatch);
            const auto rgb = ui::themes[i].accent;
            HBRUSH brush = ::CreateSolidBrush (RGB ((rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255));
            RECT r {0, 0, px (14), px (14)};
            ::FillRect (memory, &r, brush);
            ::DeleteObject (brush);
            ::SelectObject (memory, previous);
            MENUITEMINFOW item {};
            item.cbSize = sizeof item;
            item.fMask = MIIM_BITMAP;
            item.hbmpItem = swatch;
            ::SetMenuItemInfoW (menu, 1 + i, FALSE, &item);
        }
        if (swatch) swatches.push_back (swatch);
    }
    if (memory) ::DeleteDC (memory);
    ::ReleaseDC (m_hwnd, screen);
    RECT anchor {};
    ::GetWindowRect (m_themeBtn, &anchor);
    const int selection = ::TrackPopupMenu (menu, TPM_RETURNCMD | TPM_RIGHTALIGN,
                                             anchor.right, anchor.bottom + px (4), 0, m_hwnd, nullptr);
    ::DestroyMenu (menu);
    for (HBITMAP swatch : swatches) ::DeleteObject (swatch);
    if (selection > 0)
    {
        m_backend->setTheme (selection - 1);
        applyTheme (m_backend->theme ());
    }
}

void WinView::syncCurrentScore (bool showStatus)
{
    if (!m_backend) return;
    FILETIME time {};
    ::GetSystemTimeAsFileTime (&time);
    ULARGE_INTEGER stamp {};
    stamp.LowPart = time.dwLowDateTime;
    stamp.HighPart = time.dwHighDateTime;
    const uint64_t nowMs = (stamp.QuadPart - 116444736000000000ULL) / 10000;
    ap::ScoreSyncSnapshot snapshot;
    std::string raw;
    const bool connected = ap::readScoreSyncSnapshot (nowMs, snapshot, raw);
    const std::string session = connected ? snapshot.session : std::string {};
    if (session != m_scoreSession)
    {
        m_scoreSession = session;
        m_videoTimeline = {};
    }
    if (connected)
    {
        m_backend->setLiveScoreMetadata (snapshot.bpm, snapshot.beats,
                                         snapshot.denominator, snapshot.timeSigChanges, snapshot.measures, snapshot.tempos);
    }
    else m_backend->clearLiveScoreMetadata ();
    // Old manually linked files must not be mistaken for the current project.
    if (!m_backend->scorePath ().empty ()) m_backend->loadScore ({});
    // Heartbeat timestamps do not invalidate otherwise unchanged waveform pixels.
    const auto metadataStart = raw.find ('|', 4);
    const std::string content = metadataStart == std::string::npos ? raw : raw.substr (metadataStart);
    if (content != m_scoreSnapshot)
    {
        m_scoreSnapshot = content;
        m_lastPaintKey = 1.0e30;
    }
    if (!showStatus) return;
    m_backend->setFollowScore (true);
    refreshLabels ();
    requestWaveRepaint (false);
    if (!connected)
        ::MessageBoxW (m_hwnd,
            L"尚未连接当前谱面。请在 MuseScore 的插件管理中启用 MuseSync Bridge，"
            L"然后从插件菜单运行一次。之后切换工程、修改速度和拍号会自动同步，无需导入或保存谱子。",
            L"MuseSync", MB_OK | MB_ICONINFORMATION);
}

// 新载入音频的默认视野：放大到「前几小节」，从起始偏移处开始看，而不是整曲全览
// （全览时波峰糊成一片，看不清鼓点）。与 macOS 端的 `zoomToSpan:` 同一套。
// ⚠️ 这段规则被 afterLoad（用户换文件）和 restoreViewState（后端里没存过视野）
//    两处共用，所以单独成函数 —— 写两份必然漂移。
void WinView::applyDefaultView ()
{
    const double dur = m_backend->durationSec ();
    if (dur > 0.0 && dur <= kDefaultViewSpanSec)
    {
        m_viewStart = 0.0;                  // 本来就短，直接全览
        m_viewSpan  = dur;
    }
    else
    {
        m_viewSpan  = kDefaultViewSpanSec;
        m_viewStart = m_backend->offsetSec ();
        clampView ();
    }
}

// ⭐ 视图重建后把后端里存的视野读回来。
// 用户实测：「把插件界面关掉再打开，波形变成全曲全览了，而不是之前放大的那一段」。
// 根因：关掉编辑器只销毁视图窗口，后端（Processor）还活着 —— 视野是用户的操作
// 结果，跟 BPM 一样必须存在后端里，重建视图时回读。
void WinView::restoreViewState ()
{
    if (!m_backend || !m_backend->hasAudio ())
        return;

    double vs = 0.0, span = 0.0;
    m_backend->getViewState (vs, span);
    if (span > 0.0)
    {
        m_viewSpan  = span;
        m_viewStart = vs;
        clampView ();
    }
    else
    {
        applyDefaultView ();   // 后端里没存过（或刚换过音频）→ 默认视野
    }

    // 记成「已回存」，免得 onTimer 第一帧把同一份值再写一遍。
    m_pushedViewStart = m_viewStart;
    m_pushedViewSpan  = m_viewSpan;
    requestWaveRepaint (false);
}

void WinView::afterLoad ()
{
    if (!m_backend || !m_backend->hasAudio ())
        return;

    applyDefaultView ();

    // 换了音频 = 换了时间轴：上一首的谱面位置不能拿来判「宿主跳变」。
    m_hasPrevPlayhead = false;
    m_userScrolling = false;
    refreshLabels ();
    requestWaveRepaint (false);
}

void WinView::onDropFiles (HDROP drop)
{
    const UINT n = ::DragQueryFileW (drop, 0xFFFFFFFF, nullptr, 0);
    if (n > 0)
    {
        const UINT length = ::DragQueryFileW (drop, 0, nullptr, 0);
        if (length > 0)
        {
            std::vector<wchar_t> path (static_cast<size_t> (length) + 1);
            if (::DragQueryFileW (drop, 0, path.data (), static_cast<UINT> (path.size ())) > 0)
            {
                if (isVideoFile (path.data ()))
                    loadVideo (path.data (), true);
                else if (m_backend)
                    loadAudio (path.data ());
            }
        }
    }
    ::DragFinish (drop);
}

void WinView::openFileDialog ()
{
    wchar_t path[MAX_PATH * 2] = {0};
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = m_hwnd;
    ofn.lpstrFilter = L"音频文件\0*.mp3;*.wav;*.m4a;*.aac;*.wma;*.flac;*.aif;*.aiff;*.ogg;*.oga\0所有文件\0*.*\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH * 2;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;

    if (::GetOpenFileNameW (&ofn) && m_backend)
    {
        if (isVideoFile (path)) loadVideo (path, true);
        else loadAudio (path);
    }
}

void WinView::openVideoFileDialog ()
{
    wchar_t path[MAX_PATH * 2] = {};
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = m_hwnd;
    ofn.lpstrFilter = L"支持的视频文件\0*.mp4;*.webm;*.m4v\0所有文件\0*.*\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH * 2;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (::GetOpenFileNameW (&ofn))
        loadVideo (path, true);
}

void WinView::openVideoUrlDialog ()
{
    std::wstring url;
    if (!promptVideoUrl (m_hwnd, url, ui::themes[ui::clampTheme (m_theme)],
                         m_scale, m_font, m_fontBold, m_fontIcons)) return;
    loadVideo (url, false);
}

void WinView::loadVideo (const std::wstring& target, bool local)
{
    if (!m_backend || m_destroying) return;
    auto candidate = std::make_unique<VideoView> ();
    if (!(local ? candidate->openLocal (target) : candidate->openUrl (target)))
    {
        ::MessageBoxW (m_hwnd,
            L"无法打开视频。请检查文件或网址，并确认已安装 Microsoft Edge WebView2 Runtime。",
            L"MuseSync 视频", MB_OK | MB_ICONERROR);
        return;
    }
    if (m_collapsed) setCollapsed (false);
    finishProgress (true);
    if (m_videoView) m_videoView->close ();
    m_backend->unloadFile ();
    m_videoTarget = target;
    m_videoView = std::move (candidate);
    m_backend->setVideoAudio (m_videoView->audioStream ());
    if (auto analyzer = m_backend->tempoAnalyzer (); local && analyzer)
    {
        const auto path = wideToUtf8 (target);
        analyzer->decode ([path] (AudioData& audio, std::string& error,
                                 const std::function<bool ()>& cancelled) {
            const HRESULT init = ::CoInitializeEx (nullptr, COINIT_MULTITHREADED);
            struct ComScope {
                HRESULT init;
                ~ComScope () { if (SUCCEEDED (init)) ::CoUninitialize (); }
            } scope {init};
            return decodeAudioFileCancellable (path, audio, error, cancelled);
        });
    }
    m_videoSyncInitialized = false;
    m_videoTimeline = {};
    syncCurrentScore (false);
    presentMedia ();
    syncVideoToTimeline (true);
    refreshLabels ();
}

void WinView::loadAudio (const std::wstring& path)
{
    if (!m_backend || m_destroying) return;
    if (!m_backend->loadFile (wideToUtf8 (path)))
    {
        ::MessageBoxW (m_hwnd, utf8ToWide (m_backend->lastError ()).c_str (),
                       L"MuseSync 音频", MB_OK | MB_ICONERROR);
        return;
    }
    if (m_collapsed) setCollapsed (false);
    finishProgress (true);
    if (m_videoView) m_videoView->close ();
    m_videoView.reset ();
    m_backend->setVideoAudio (nullptr);
    m_videoTarget.clear ();
    m_videoSyncInitialized = false;
    presentMedia ();
    afterLoad ();
}

void WinView::closeMedia ()
{
    if (!m_backend) return;
    if (m_collapsed) setCollapsed (false);
    finishProgress (true);
    m_backend->setVideoAudio (nullptr);
    if (m_videoView) m_videoView->close ();
    m_videoView.reset ();
    m_videoTarget.clear ();
    m_videoSyncInitialized = false;
    m_backend->unloadFile ();
    m_hasPrevPlayhead = false;
    presentMedia ();
    refreshLabels ();
    requestWaveRepaint (false);
}

void WinView::applyBpmFromEdit ()
{
    if (!m_backend || !m_bpmEdit || m_backend->followScore ())
        return;
    wchar_t t[32] = {0};
    ::GetWindowTextW (m_bpmEdit, t, 32);
    const double v = ::_wtof (t);
    m_backend->setGridBPM (v > 0.0 ? static_cast<float> (v) : 0.0f);
    refreshLabels ();
    requestWaveRepaint (false);
}

void WinView::applyPitchFromEdit (int id)
{
    if (!m_backend || m_syncingControls) return;
    const bool semitone = id == IDC_SEMITONE_EDIT;
    wchar_t text[32] {};
    ::GetWindowTextW (semitone ? m_semitoneEdit : m_centsEdit, text, 32);
    double value = 0;
    if (ui::parseNumericInput (text, semitone ? -24 : -100, semitone ? 24 : 100, value) &&
        value == std::round (value))
    {
        if (semitone) m_backend->setSemitones (static_cast<int> (value));
        else m_backend->setCents (static_cast<int> (value));
    }
    refreshLabels ();
    requestWaveRepaint (false);
}

void WinView::syncControlsFromBackend ()
{
    if (!m_backend)
        return;
    m_syncingControls = true;

    if (m_bpmEdit)
    {
        const float bpm = m_backend->gridBPM ();
        if (bpm >= 1.0f)
        {
            wchar_t t[32] = {0};
            std::swprintf (t, 32, L"%d", static_cast<int> (bpm + 0.5f));
            ::SetWindowTextW (m_bpmEdit, t);
        }
        else
        {
            // 空框 = 自动（优先跟随宿主速度，宿主不给就用 120）。
            // 不再预填 "120"：那会让「用户还没设过」看起来像「已经设成 120」，
            // 而且一提交就真的变成手动 120 —— 与 macOS 端的语义对齐。
            ::SetWindowTextW (m_bpmEdit, L"");
        }
    }

    if (m_timesig)
    {
        const int beats = m_backend->gridBeatsPerBar ();
        const int denom = m_backend->gridBeatDenominator ();
        for (int i = 0; i < kTimeSigCount; ++i)
        {
            if (kTimeSigs[i].beats == beats && kTimeSigs[i].denom == denom)
            {
                ::SendMessageW (m_timesig, CB_SETCURSEL, i, 0);
                break;
            }
        }
    }

    if (m_volSlider)
        ::SendMessageW (m_volSlider, TBM_SETPOS, TRUE,
                        static_cast<LPARAM> (static_cast<int> (m_backend->volume () * 100.0f + 0.5f)));

    if (m_semitoneEdit)
    {
        wchar_t t[32];
        std::swprintf (t, 32, L"%d", m_backend->semitones ());
        ::SetWindowTextW (m_semitoneEdit, t);
    }
    if (m_centsEdit)
    {
        wchar_t t[32];
        std::swprintf (t, 32, L"%d", m_backend->cents ());
        ::SetWindowTextW (m_centsEdit, t);
    }
    m_syncingControls = false;
}

void WinView::applyNumericEdit (int id)
{
    if (!m_backend) return;
    wchar_t text[64] {};
    HWND control = m_offsetEdit;
    ::GetWindowTextW (control, text, 64);
    double value = 0;
    if (ui::parseNumericInput (text, -3600000.0, 3600000.0, value))
    {
        m_backend->setOffsetSec (static_cast<float> (value / 1000.0));
    }
    refreshLabels ();
    requestWaveRepaint (false);
}

void WinView::onHScroll (HWND slider)
{
    if (!m_backend || !m_volSlider || slider != m_volSlider)
        return;
    const int pos = static_cast<int> (::SendMessageW (m_volSlider, TBM_GETPOS, 0, 0));
    m_backend->setVolume (static_cast<float> (pos) / 100.0f);
    refreshLabels ();
}

bool WinView::onNumericWheel (HWND control, int delta, bool fine, bool coarse)
{
    if (!m_backend || !control || (!::IsChild (m_hwnd, control) &&
        (!m_floating || !::IsChild (m_floating, control)))) return false;
    const int id = ::GetDlgCtrlID (control);
    if (id != IDC_SEMITONE_EDIT && id != IDC_CENTS_EDIT && id != IDC_BPM_EDIT &&
        id != IDC_OFFSET_EDIT && id != IDC_VOLUME_SLIDER) return false;
    if (id == IDC_BPM_EDIT && m_backend->followScore ()) return true;
    if (m_wheelControl != control)
    {
        m_wheelControl = control;
        m_numericWheelRemainder = 0;
    }
    m_numericWheelRemainder += delta;
    const int steps = m_numericWheelRemainder / WHEEL_DELTA;
    m_numericWheelRemainder %= WHEEL_DELTA;
    if (!steps) return true;
    double value = 0;
    wchar_t text[64] {};
    ::GetWindowTextW (control, text, 64);
    const double min = id == IDC_OFFSET_EDIT ? -3600000 : id == IDC_SEMITONE_EDIT ? -24 :
        id == IDC_CENTS_EDIT ? -100 : id == IDC_BPM_EDIT ? 20 : 0;
    const double max = id == IDC_OFFSET_EDIT ? 3600000 : id == IDC_SEMITONE_EDIT ? 24 :
        id == IDC_CENTS_EDIT ? 100 : id == IDC_BPM_EDIT ? 400 : 100;
    if (!ui::parseNumericInput (text, min, max, value))
        value = id == IDC_OFFSET_EDIT ? m_backend->offsetSec () * 1000.0 :
            id == IDC_SEMITONE_EDIT ? m_backend->semitones () : id == IDC_CENTS_EDIT ? m_backend->cents () :
            id == IDC_BPM_EDIT ? ap::resolveGridMetadata (*m_backend).bpm : m_backend->volume () * 100.0;
    const double increment = id == IDC_OFFSET_EDIT ? (coarse ? 100 : fine ? 1 : 10) : 1;
    value = std::clamp (std::round (value) + steps * increment, min, max);
    m_syncingControls = true;
    std::swprintf (text, 64, L"%.0f", value);
    if (id != IDC_VOLUME_SLIDER) ::SetWindowTextW (control, text);
    m_syncingControls = false;
    if (id == IDC_OFFSET_EDIT) m_backend->setOffsetSec (static_cast<float> (value / 1000));
    else if (id == IDC_SEMITONE_EDIT) m_backend->setSemitones (static_cast<int> (value));
    else if (id == IDC_CENTS_EDIT) m_backend->setCents (static_cast<int> (value));
    else if (id == IDC_BPM_EDIT) m_backend->setGridBPM (static_cast<float> (value));
    else m_backend->setVolume (static_cast<float> (value / 100));
    refreshLabels ();
    requestWaveRepaint (false);
    return true;
}

void WinView::paintSlider (HWND slider, HDC dc)
{
    RECT r {};
    ::GetClientRect (slider, &r);
    ::FillRect (dc, &r, m_bgBrush);
    const int inset = px (8), mid = (r.top + r.bottom) / 2;
    const bool progress = slider == m_progressSlider;
    const int value = static_cast<int> (::SendMessageW (slider, TBM_GETPOS, 0, 0));
    const bool opacity = slider == m_opacitySlider;
    const int thumb = inset + (r.right - 2 * inset) *
        (opacity ? value - 20 : value) / (progress ? 10000 : opacity ? 80 : 100);
    RECT track {inset, mid - px (2), r.right - inset, mid + px (2)};
    HBRUSH brush = ::CreateSolidBrush (kRule);
    ::FillRect (dc, &track, brush);
    ::DeleteObject (brush);
    track.right = thumb;
    brush = ::CreateSolidBrush (!::IsWindowEnabled (slider) ? kMuted : progress ? kAccent : kInk);
    ::FillRect (dc, &track, brush);
    HGDIOBJ oldBrush = ::SelectObject (dc, brush);
    HGDIOBJ oldPen = ::SelectObject (dc, ::GetStockObject (NULL_PEN));
    ::Ellipse (dc, thumb - px (5), mid - px (5), thumb + px (5), mid + px (5));
    ::SelectObject (dc, oldPen);
    ::SelectObject (dc, oldBrush);
    ::DeleteObject (brush);
    if (::GetFocus () == slider)
    {
        ::InflateRect (&r, -px (1), -px (1));
        ::DrawFocusRect (dc, &r);
    }
}

void WinView::paintTimeSigControl (HDC dc)
{
    RECT r {};
    ::GetClientRect (m_timesig, &r);
    ::FillRect (dc, &r, m_bgBrush);
    HBRUSH brush = ::CreateSolidBrush (kSurface);
    HPEN pen = ::CreatePen (PS_SOLID, std::max (1, px (1)), kRule);
    HGDIOBJ oldBrush = ::SelectObject (dc, brush);
    HGDIOBJ oldPen = ::SelectObject (dc, pen);
    ::RoundRect (dc, r.left, r.top, r.right, r.bottom, px (4), px (4));
    ::SelectObject (dc, oldPen);
    ::SelectObject (dc, oldBrush);
    ::DeleteObject (pen);
    ::DeleteObject (brush);

    const int selection = static_cast<int> (::SendMessageW (m_timesig, CB_GETCURSEL, 0, 0));
    const wchar_t* value = m_backend && m_backend->followScore () ? m_meterText.c_str () :
        selection >= 0 && selection < kTimeSigCount ? kTimeSigs[selection].label : L"--";
    RECT label = r;
    label.left += px (12);
    label.right -= px (32);
    HGDIOBJ oldFont = ::SelectObject (dc, m_fontMono);
    ::SetBkMode (dc, TRANSPARENT);
    ::SetTextColor (dc, kInk);
    ::DrawTextW (dc, value, -1, &label, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    RECT arrow = r;
    arrow.left = arrow.right - px (28);
    ::SelectObject (dc, m_fontIcons);
    ::SetTextColor (dc, kMuted);
    ::DrawTextW (dc, m_backend && m_backend->followScore () ? L"\xE72E" : L"\xE70D", -1, &arrow,
                 DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    ::SelectObject (dc, oldFont);
    if (::GetFocus () == m_timesig)
    {
        ::InflateRect (&r, -px (3), -px (3));
        ::DrawFocusRect (dc, &r);
    }
}

void WinView::setVolumeAt (int x)
{
    RECT r {};
    ::GetClientRect (m_volSlider, &r);
    const int inset = px (8);
    const int value = std::clamp (static_cast<int> (std::lround (
        100.0 * (x - inset) / std::max (1L, r.right - 2 * inset))), 0, 100);
    ::SendMessageW (m_volSlider, TBM_SETPOS, TRUE, value);
    onHScroll (m_volSlider);
    ::InvalidateRect (m_volSlider, nullptr, FALSE);
}

void WinView::seekFromAudio (double audioSec)
{
    if (!m_backend || (!m_videoView && !m_backend->hasAudio ())) return;
    const double scoreSec = std::max (0.0, audioSec - m_backend->offsetSec ());
    if (!ap::requestScoreSeek (scoreSec, ap::scoreBridgeNowMs ()))
        ::MessageBoxW (m_hwnd,
            L"谱面跳转需要新版 MuseSync Bridge。请重启 MuseScore，再从插件菜单运行 MuseSync Bridge。",
            L"MuseSync", MB_OK | MB_ICONINFORMATION);
    else
    {
        m_userScrolling = false;
        m_lastUserScrollTick = 0;
    }
}

void WinView::setProgressAt (int x)
{
    if (!m_backend || !::IsWindowEnabled (m_progressSlider)) return;
    RECT r {};
    ::GetClientRect (m_progressSlider, &r);
    const int inset = px (8);
    const int value = static_cast<int> (std::lround (std::clamp (
        (x - inset) / static_cast<double> (std::max (1L, r.right - 2 * inset)), 0.0, 1.0) * 10000));
    m_progressDragging = true;
    ::SendMessageW (m_progressSlider, TBM_SETPOS, FALSE, value);
    ::InvalidateRect (m_progressSlider, nullptr, FALSE);
    refreshLabels ();
}

void WinView::finishProgress (bool cancel)
{
    if (!m_progressDragging) return;
    m_progressDragging = false;
    const double target = mediaDuration () *
        ::SendMessageW (m_progressSlider, TBM_GETPOS, 0, 0) / 10000.0;
    if (::GetCapture () == m_progressSlider) ::ReleaseCapture ();
    // Commit once on release so the companion is not flooded while scrubbing.
    if (!cancel) seekFromAudio (target);
    refreshLabels ();
}

bool WinView::onProgressKey (WPARAM key)
{
    if (key == VK_ESCAPE && m_progressDragging) { finishProgress (true); return true; }
    if (key != VK_LEFT && key != VK_RIGHT && key != VK_UP && key != VK_DOWN &&
        key != VK_HOME && key != VK_END && key != VK_PRIOR && key != VK_NEXT) return false;
    if (!m_backend || !::IsWindowEnabled (m_progressSlider)) return true;
    const int step = static_cast<int> (std::lround (std::clamp (
        10000 / mediaDuration (), 1.0, 10000.0)));
    int value = static_cast<int> (::SendMessageW (m_progressSlider, TBM_GETPOS, 0, 0));
    if (key == VK_HOME) value = 0;
    else if (key == VK_END) value = 10000;
    else value += key == VK_LEFT || key == VK_UP ? -step :
                  key == VK_RIGHT || key == VK_DOWN ? step : key == VK_PRIOR ? -500 : 500;
    ::SendMessageW (m_progressSlider, TBM_SETPOS, FALSE, std::clamp (value, 0, 10000));
    m_progressDragging = true;
    finishProgress (false);
    return true;
}

void WinView::onTempoClick (int x, int y)
{
    const auto box = m_layout.tempoMap;
    const double dx = x / m_scale, dy = y / m_scale;
    if (dx >= box.x && dx < box.right () && dy >= box.y && dy < box.bottom () && mediaDuration () > 0)
        seekFromAudio ((dx - box.x) / box.w * mediaDuration ());
}

LRESULT WinView::onNotify (NMHDR* notification)
{
    if (!notification || notification->hwndFrom != m_volSlider ||
        notification->code != NM_CUSTOMDRAW)
        return CDRF_DODEFAULT;
    auto* draw = reinterpret_cast<NMCUSTOMDRAW*> (notification);
    if (draw->dwDrawStage == CDDS_PREPAINT)
    {
        RECT client = {};
        ::GetClientRect (m_volSlider, &client);
        ::FillRect (draw->hdc, &client, m_bgBrush);
        return CDRF_NOTIFYITEMDRAW;
    }
    if (draw->dwDrawStage != CDDS_ITEMPREPAINT)
        return CDRF_DODEFAULT;
    if (draw->dwItemSpec == TBCD_CHANNEL)
    {
        RECT r {};
        ::SendMessageW (m_volSlider, TBM_GETCHANNELRECT, 0, reinterpret_cast<LPARAM> (&r));
        const int mid = (r.top + r.bottom) / 2;
        r.top = mid - px (1);
        r.bottom = mid + px (2);
        HBRUSH track = ::CreateSolidBrush (kRule);
        ::FillRect (draw->hdc, &r, track);
        ::DeleteObject (track);
        const int volume = static_cast<int> (::SendMessageW (m_volSlider, TBM_GETPOS, 0, 0));
        r.right = r.left + (r.right - r.left) * volume / 100;
        HBRUSH fill = ::CreateSolidBrush (kInk);
        ::FillRect (draw->hdc, &r, fill);
        ::DeleteObject (fill);
    }
    else if (draw->dwItemSpec == TBCD_THUMB)
    {
        RECT r {};
        ::SendMessageW (m_volSlider, TBM_GETTHUMBRECT, 0, reinterpret_cast<LPARAM> (&r));
        const int midX = (r.left + r.right) / 2;
        const int midY = (r.top + r.bottom) / 2;
        HBRUSH fill = ::CreateSolidBrush (kInk);
        HGDIOBJ oldBrush = ::SelectObject (draw->hdc, fill);
        HGDIOBJ oldPen = ::SelectObject (draw->hdc, ::GetStockObject (NULL_PEN));
        ::Ellipse (draw->hdc, midX - px (6), midY - px (6), midX + px (6), midY + px (6));
        ::SelectObject (draw->hdc, oldPen);
        ::SelectObject (draw->hdc, oldBrush);
        ::DeleteObject (fill);
        if (::GetFocus () == m_volSlider)
            ::DrawFocusRect (draw->hdc, &r);
    }
    else return CDRF_DODEFAULT;
    return CDRF_SKIPDEFAULT;
}

HBRUSH WinView::onCtlColor (HDC dc, HWND ctl)
{
    ::SetTextColor (dc, kInk);
    ::SetBkMode (dc, OPAQUE);
    ::SetBkColor (dc, kSurface);
    if (ctl == m_bpmEdit || ctl == m_semitoneEdit || ctl == m_centsEdit ||
        ctl == m_offsetEdit ||
        ctl == m_timesig)
        return m_inputBrush;
    if (ctl == m_volSlider || ctl == m_progressSlider)
    {
        ::SetBkColor (dc, kPaper);
        return m_bgBrush;
    }
    return m_inputBrush;
}

// MuseScore 的播放快捷键通常由顶层窗口处理。插件窗口可能嵌在一层或多层
// VST 容器中，因此这里沿父链取根窗口，再发送一次完整的按下/抬起序列。
bool forwardSpaceToHost (WinView* v, LPARAM lp)
{
    if (!v || !v->hwnd () ||
        (static_cast<ULONG_PTR> (lp) & (static_cast<ULONG_PTR> (1) << 30)) != 0)
        return false;

    HWND root = ::GetAncestor (v->hwnd (), GA_ROOT);
    if (root && !::IsWindowVisible (root) && ::GetWindow (root, GW_OWNER))
        root = ::GetAncestor (::GetWindow (root, GW_OWNER), GA_ROOT);
    if (!root || root == v->hwnd ())
        return false;

    ::PostMessageW (root, WM_KEYDOWN, VK_SPACE, lp);
    ::PostMessageW (root, WM_KEYUP, VK_SPACE, lp);
    return true;
}

static LRESULT CALLBACK spaceSyncSubclassProc (HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                                               UINT_PTR /*id*/, DWORD_PTR ref)
{
    WinView* v = reinterpret_cast<WinView*> (ref);
    const int controlId = ::GetDlgCtrlID (hwnd);
    // Only replace the closed face; the native combo retains its popup and keys.
    if (v && controlId == IDC_TIMESIG_COMBO)
    {
        if (msg == WM_PAINT)
        {
            PAINTSTRUCT ps {};
            HDC dc = ::BeginPaint (hwnd, &ps);
            v->paintTimeSigControl (dc);
            ::EndPaint (hwnd, &ps);
            return 0;
        }
        if (msg == WM_PRINTCLIENT && wp)
        {
            v->paintTimeSigControl (reinterpret_cast<HDC> (wp));
            return 0;
        }
        if (msg == WM_ERASEBKGND) return 1;
        if (msg == WM_SETFOCUS || msg == WM_KILLFOCUS || msg == WM_ENABLE)
            ::InvalidateRect (hwnd, nullptr, FALSE);
    }
    if (v && (controlId == IDC_VOLUME_SLIDER || controlId == IDC_PROGRESS_SLIDER ||
              controlId == IDC_VIDEO_OPACITY))
    {
        const bool progress = controlId == IDC_PROGRESS_SLIDER;
        const bool opacity = controlId == IDC_VIDEO_OPACITY;
        auto setAt = [&] (int x) {
            if (progress) v->setProgressAt (x);
            else if (opacity)
            {
                RECT bounds {};
                ::GetClientRect (hwnd, &bounds);
                const int inset = scaledOf (8);
                const int value = std::clamp (20 + static_cast<int> (std::lround (
                    80.0 * (x - inset) / std::max (1L, bounds.right - 2 * inset))), 20, 100);
                ::SendMessageW (hwnd, TBM_SETPOS, TRUE, value);
                v->setVideoOpacity ();
                ::InvalidateRect (hwnd, nullptr, FALSE);
            }
            else v->setVolumeAt (x);
        };
        if (msg == WM_PAINT)
        {
            PAINTSTRUCT ps {};
            HDC dc = ::BeginPaint (hwnd, &ps);
            v->paintSlider (hwnd, dc);
            ::EndPaint (hwnd, &ps);
            return 0;
        }
        if (msg == WM_PRINTCLIENT && wp)
        {
            v->paintSlider (hwnd, reinterpret_cast<HDC> (wp));
            return 0;
        }
        if (msg == WM_ERASEBKGND) return 1;
        if (progress && msg == WM_KEYDOWN && v->onProgressKey (wp)) return 0;
        if (progress && (msg == WM_CAPTURECHANGED || msg == WM_CANCELMODE))
            v->finishProgress (true);
        if (progress && msg == WM_MOUSEWHEEL) return 0;
        if (msg == WM_LBUTTONDOWN)
        {
            if (!::IsWindowEnabled (hwnd)) return 0;
            ::SetFocus (hwnd);
            ::SetCapture (hwnd);
            setAt (GET_X_LPARAM (lp));
            return 0;
        }
        if (msg == WM_MOUSEMOVE && ::GetCapture () == hwnd)
        {
            setAt (GET_X_LPARAM (lp));
            return 0;
        }
        if (msg == WM_LBUTTONUP && ::GetCapture () == hwnd)
        {
            if (progress)
            {
                v->setProgressAt (GET_X_LPARAM (lp));
                v->finishProgress (false);
            }
            else
            {
                setAt (GET_X_LPARAM (lp));
                ::ReleaseCapture ();
            }
            return 0;
        }
        if (msg == WM_SETFOCUS || msg == WM_KILLFOCUS || msg == WM_ENABLE)
            ::InvalidateRect (hwnd, nullptr, FALSE);
    }
    if (v && msg == WM_MOUSEWHEEL)
    {
        POINT point {GET_X_LPARAM (lp), GET_Y_LPARAM (lp)};
        HWND target = ::WindowFromPoint (point);
        if (!target || (::GetAncestor (target, GA_ROOT) != ::GetAncestor (hwnd, GA_ROOT))) target = hwnd;
        if (v->onNumericWheel (target, GET_WHEEL_DELTA_WPARAM (wp),
                               (GET_KEYSTATE_WPARAM (wp) & MK_SHIFT) != 0,
                               (GET_KEYSTATE_WPARAM (wp) & MK_CONTROL) != 0)) return 0;
    }
    if (v && msg == WM_MOUSEMOVE)
    {
        v->hoverControl (hwnd);
        TRACKMOUSEEVENT tracking = {sizeof tracking, TME_LEAVE, hwnd, 0};
        ::TrackMouseEvent (&tracking);
    }
    else if (v && msg == WM_MOUSELEAVE)
        v->hoverControl (nullptr);
    if (v && msg == WM_KEYDOWN && wp == VK_RETURN)
    {
        const int id = ::GetDlgCtrlID (hwnd);
        if (id == IDC_BPM_EDIT || id == IDC_SEMITONE_EDIT || id == IDC_CENTS_EDIT ||
            id == IDC_OFFSET_EDIT)
        {
            ::SetFocus (v->hwnd ());
            return 0;
        }
    }
    const bool numeric = controlId == IDC_BPM_EDIT || controlId == IDC_SEMITONE_EDIT ||
        controlId == IDC_CENTS_EDIT || controlId == IDC_OFFSET_EDIT;
    auto validReplacement = [&] (const std::wstring& inserted) {
        wchar_t text[64] {};
        ::GetWindowTextW (hwnd, text, 64);
        DWORD start = 0, end = 0;
        ::SendMessageW (hwnd, EM_GETSEL, reinterpret_cast<WPARAM> (&start), reinterpret_cast<LPARAM> (&end));
        std::wstring proposed (text);
        proposed.replace (std::min<size_t> (start, proposed.size ()), end - start, inserted);
        if (controlId == IDC_BPM_EDIT && proposed.find_first_not_of (L"0123456789") != std::wstring::npos)
            return false;
        return ui::numericDraft (proposed.c_str ());
    };
    if (numeric && msg == WM_GETDLGCODE)
        return ::DefSubclassProc (hwnd, msg, wp, lp) | DLGC_WANTALLKEYS;
    // TranslateMessage queues WM_CHAR before the key handler changes focus.
    // Consume it too, so the single-line EDIT cannot beep or insert a space.
    if (numeric && msg == WM_CHAR)
    {
        if (wp == VK_RETURN || wp == VK_ESCAPE || wp == VK_SPACE) return 0;
        if (wp >= 32 && !validReplacement (std::wstring (1, static_cast<wchar_t> (wp))))
            return 0;
    }
    if (numeric && msg == WM_PASTE)
    {
        bool valid = false;
        if (::OpenClipboard (hwnd))
        {
            if (HANDLE data = ::GetClipboardData (CF_UNICODETEXT))
            {
                if (const auto* text = static_cast<const wchar_t*> (::GlobalLock (data)))
                {
                    double value = 0;
                    valid = ui::parseNumericInput (text, -3600000, 3600000, value) && validReplacement (text);
                    ::GlobalUnlock (data);
                }
            }
            ::CloseClipboard ();
        }
        if (!valid) return 0;
    }
    if (msg == WM_CHAR && wp == VK_SPACE) return 0;
    if ((msg == WM_KEYDOWN || msg == WM_KEYUP) && wp == VK_SPACE)
    {
        if (msg == WM_KEYDOWN)
            forwardSpaceToHost (v, lp);
        // 不让按钮的空格激活、不让编辑框插入空格；宿主已经收到成对按键。
        return 0;
    }
    return ::DefSubclassProc (hwnd, msg, wp, lp);
}

/// SetWindowSubclass 的子类 ID 见文件前半的前置声明区（必须在 WinView::create 之前定义）
void WinView::onCommand (int id, int notify)
{
    switch (id)
    {
        case IDC_CLOSE_MEDIA:
            closeMedia ();
            break;
        case IDC_UPDATE_CHECK:
            m_updater.check ();
            refreshUpdates ();
            break;
        case IDC_UPDATE_ACTION:
            updateAction ();
            break;
        case IDC_OPEN:
            openFileDialog ();
            break;
        case IDC_OPEN_VIDEO:
        {
            HMENU menu = ::CreatePopupMenu ();
            if (!menu) { openVideoFileDialog (); break; }
            ::AppendMenuW (menu, MF_STRING, IDC_OPEN_VIDEO_LOCAL, L"打开本地视频...");
            ::AppendMenuW (menu, MF_STRING, IDC_OPEN_VIDEO_URL, L"打开网络视频...");
            RECT rc {};
            ::GetWindowRect (m_videoBtn, &rc);
            const int choice = static_cast<int> (::TrackPopupMenu (menu,
                TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_TOPALIGN, rc.left, rc.bottom,
                0, m_hwnd, nullptr));
            ::DestroyMenu (menu);
            if (choice == IDC_OPEN_VIDEO_LOCAL) openVideoFileDialog ();
            else if (choice == IDC_OPEN_VIDEO_URL) openVideoUrlDialog ();
            break;
        }
        case IDC_OPEN_VIDEO_LOCAL:
            openVideoFileDialog ();
            break;
        case IDC_OPEN_VIDEO_URL:
            openVideoUrlDialog ();
            break;
        case IDC_LINK_SCORE:
            syncCurrentScore (true);
            break;
        case IDC_THEME_BTN:
            showThemeMenu ();
            break;
        case IDC_FOLLOW_SCORE:
            if (m_backend)
            {
                const auto grid = ap::resolveGridMetadata (*m_backend);
                m_backend->setFollowScore (!m_backend->followScore ());
                if (!m_backend->followScore () && m_backend->gridBPM () <= 0)
                {
                    m_backend->setGridBPM (grid.bpm);
                    m_backend->setGridBeatsPerBar (grid.beats);
                    m_backend->setGridBeatDenominator (grid.denominator);
                }
                syncControlsFromBackend ();
                refreshLabels ();
                ::InvalidateRect (m_followScoreCheck, nullptr, FALSE);
                requestWaveRepaint (false);
            }
            break;
        case IDC_ZOOM_OUT:
            zoomBy (1.25, m_backend ? m_backend->positionSec () : 0.0);
            requestWaveRepaint (false);
            break;
        case IDC_ZOOM_IN:
            zoomBy (0.8, m_backend ? m_backend->positionSec () : 0.0);
            requestWaveRepaint (false);
            break;
        case IDC_ZOOM_FIT:
            zoomFit ();
            requestWaveRepaint (false);
            break;
        case IDC_BACK_TO_SCORE:
            if (m_videoView) syncVideoToTimeline (true);
            if (m_backend && m_backend->hasAudio ())
            {
                const PlugView::Backend::HostTimeline tl = m_backend->hostTimeline ();
                if (tl.playheadValid)
                {
                    // ⚠ 必须带上起始偏移：谱面 0 秒 = 音频 offset 秒。
                    //   旧代码写的是 seekTo (tl.playheadSec)，漏了这个偏移 ——
                    //   点一次就把两条播放头按偏移量错开，恰好和这个按钮
                    //   「修复分家」的职责相反。（macOS 端一直是带偏移的。）
                    const double target = tl.playheadSec + m_backend->offsetSec ();
                    m_backend->seekTo (target);
                    centerViewOn (target);      // 视野也回到播放位置

                    // 点这个按钮 = 「我要看播放头」，所以立刻恢复自动跟随
                    //（否则刚滚过波形的人点了按钮还是 2 秒不跟，白点）。
                    m_userScrolling = false;
                    m_lastUserScrollTick = 0;

                    // 记下「刚刚手动对齐过」：若 3 秒内 onTimer 又发现错位，
                    // 说明是跟随失效而非一次性偏差 → 徽标升级提示 + 落一条日志。
                    m_lastBackTick = ::GetTickCount ();
                    m_followFail = false;
                }
            }
            refreshLabels ();
            requestWaveRepaint (false);
            break;
        case IDC_OFFSET_ZERO:
            // 「归零」：把起始偏移一键清零。
            // 之前唯一的改偏移入口是「在波形上 Ctrl/Alt 拖动」，手滑拖到很大的值
            // 之后只能反向拖回去 —— 可能要拖好几个屏。用户实测点名要这个按钮。
            if (m_backend)
            {
                m_backend->setOffsetSec (0.0f);

                // 归零会让音频跳回「谱面位置 + 0」。若播放头因此跑出画面，就顺手把
                // 它带回来（只在真跑出画面时才动 —— 用户正盯着的那段不该被抢走）。
                const PlugView::Backend::HostTimeline tlb = m_backend->hostTimeline ();
                if (tlb.playheadValid &&
                    (tlb.playheadSec < m_viewStart ||
                     tlb.playheadSec > m_viewStart + m_viewSpan))
                    centerViewOn (tlb.playheadSec);

                refreshLabels ();
                requestWaveRepaint (false);
            }
            break;
        case IDC_HELP_BTN:
            toggleHelp ();
            break;
        case IDC_COLLAPSE:
            setCollapsed (true);
            break;
        case IDC_EXPAND:
            setCollapsed (false);
            break;
        case IDC_GRID_CHECK:
            m_showGrid = !m_showGrid;
            ::InvalidateRect (m_gridCheck, nullptr, FALSE);
            requestWaveRepaint (false);
            break;
        case IDC_NUM_CHECK:
            m_showNumbers = !m_showNumbers;
            ::InvalidateRect (m_numCheck, nullptr, FALSE);
            requestWaveRepaint (false);
            break;
        case IDC_BPM_UP:
        case IDC_BPM_DOWN:
        {
            if (!m_backend || m_backend->followScore ()) break;
            // 两步合并：逻辑完全相同，只差符号。
            // ⚠️ 必须先做下限钳制再看符号 —— 空框（= 自动）时 _wtof("") 得 0，
            //    旧写法会算出「1 BPM」这种荒唐值。现在空框按 20 BPM 起步，
            //    与 macOS 端步进器的范围（20~400）一致。
            if (m_bpmEdit)
            {
                wchar_t t[32] = {0};
                ::GetWindowTextW (m_bpmEdit, t, 32);
                double v = ::_wtof (t);
                if (v < 20.0) v = 20.0;
                v += (id == IDC_BPM_UP) ? 1.0 : -1.0;
                if (v < 20.0)  v = 20.0;
                if (v > 400.0) v = 400.0;
                std::swprintf (t, 32, L"%d", static_cast<int> (v));
                ::SetWindowTextW (m_bpmEdit, t);
                applyBpmFromEdit ();
            }
            break;
        }
        case IDC_BPM_EDIT:
            if (notify == EN_KILLFOCUS)
                applyBpmFromEdit ();
            break;
        case IDC_OFFSET_EDIT:
            if (notify == EN_KILLFOCUS) applyNumericEdit (id);
            break;
        case IDC_OFFSET_DOWN:
        case IDC_OFFSET_UP:
            if (m_backend)
            {
                const double increment = (::GetKeyState (VK_SHIFT) & 0x8000) ? 1 : 10;
                const double ms = std::clamp (std::round (m_backend->offsetSec () * 1000.0) +
                    (id == IDC_OFFSET_UP ? increment : -increment), -3600000.0, 3600000.0);
                m_backend->setOffsetSec (static_cast<float> (ms / 1000));
                refreshLabels ();
                requestWaveRepaint (false);
            }
            break;
        case IDC_SEMITONE_UP:
        case IDC_SEMITONE_DOWN:
        case IDC_CENTS_UP:
        case IDC_CENTS_DOWN:
        {
            if (!m_backend)
                break;
            const bool semitone = id == IDC_SEMITONE_UP || id == IDC_SEMITONE_DOWN;
            const int delta = (id == IDC_SEMITONE_UP || id == IDC_CENTS_UP) ? 1 : -1;
            if (semitone)
                m_backend->setSemitones (m_backend->semitones () + delta);
            else
                m_backend->setCents (m_backend->cents () + delta);
            refreshLabels ();
            requestWaveRepaint (false);
            break;
        }
        case IDC_SEMITONE_EDIT:
        case IDC_CENTS_EDIT:
            if (notify == EN_CHANGE || notify == EN_KILLFOCUS)
                applyPitchFromEdit (id);
            break;
        case IDC_PITCH_RESET:
            if (m_backend)
            {
                m_backend->setSemitones (0);
                m_backend->setCents (0);
                refreshLabels ();
                requestWaveRepaint (false);
            }
            break;
        case IDC_TIMESIG_COMBO:
            if (notify == CBN_SELCHANGE && m_backend && !m_backend->followScore ())
            {
                const int sel = static_cast<int> (::SendMessageW (m_timesig, CB_GETCURSEL, 0, 0));
                if (sel >= 0 && sel < kTimeSigCount)
                {
                    m_backend->setGridBeatsPerBar (kTimeSigs[sel].beats);
                    m_backend->setGridBeatDenominator (kTimeSigs[sel].denom);
                    requestWaveRepaint (false);
                }
            }
            break;
        default:
            break;
    }
}

//------------------------------------------------------------------------------
// 窗口过程
//------------------------------------------------------------------------------
namespace {

LRESULT CALLBACK floatingProc (HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_NCCREATE)
    {
        const auto* cs = reinterpret_cast<CREATESTRUCTW*> (lp);
        ::SetWindowLongPtrW (hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR> (cs->lpCreateParams));
    }
    WinView* v = viewOf (hwnd);
    switch (msg)
    {
        // Keep the resize style/hit targets without the system's white non-client frame.
        case WM_NCCALCSIZE:
            return 0;
        case WM_PAINT:
        {
            PAINTSTRUCT ps {};
            HDC dc = ::BeginPaint (hwnd, &ps);
            if (v) v->paintFloating (dc);
            ::EndPaint (hwnd, &ps);
            return 0;
        }
        case WM_PRINTCLIENT:
            if (v && wp) v->paintFloating (reinterpret_cast<HDC> (wp));
            return 0;
        case WM_NCHITTEST:
            return v ? v->floatingHitTest (POINT {GET_X_LPARAM (lp), GET_Y_LPARAM (lp)}) : HTCLIENT;
        case WM_SIZE:
            if (v) v->layoutFloating ();
            return 0;
        case WM_MOVE:
            if (v) v->moveFloatingVideo ();
            return 0;
        case WM_GETMINMAXINFO:
        {
            auto* info = reinterpret_cast<MINMAXINFO*> (lp);
            const bool video = (::GetWindowLongPtrW (hwnd, GWL_STYLE) & WS_THICKFRAME) != 0;
            info->ptMinTrackSize = video ? POINT {scaledOf (ui::videoFloatingMinWidth), scaledOf (ui::videoFloatingMinHeight)} :
                                          POINT {scaledOf (260), scaledOf (58)};
            return 0;
        }
        case WM_HSCROLL:
            if (v && LOWORD (::GetDlgCtrlID (reinterpret_cast<HWND> (lp))) == IDC_VIDEO_OPACITY)
                v->setVideoOpacity ();
            else if (v) v->onHScroll (reinterpret_cast<HWND> (lp));
            return 0;
        case WM_NCLBUTTONDBLCLK:
        case WM_LBUTTONDBLCLK:
        case WM_CLOSE:
            if (v) v->setCollapsed (false);
            return 0;
        case WM_COMMAND:
            if (v) v->onCommand (LOWORD (wp), HIWORD (wp));
            return 0;
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORSTATIC:
            if (v) return reinterpret_cast<LRESULT> (v->onCtlColor (
                reinterpret_cast<HDC> (wp), reinterpret_cast<HWND> (lp)));
            break;
        case WM_MOUSEWHEEL:
            if (v && v->onNumericWheel (::WindowFromPoint ({GET_X_LPARAM (lp), GET_Y_LPARAM (lp)}),
                GET_WHEEL_DELTA_WPARAM (wp), (GET_KEYSTATE_WPARAM (wp) & MK_SHIFT) != 0,
                (GET_KEYSTATE_WPARAM (wp) & MK_CONTROL) != 0)) return 0;
            break;
        case WM_DRAWITEM:
            if (v && v->drawItem (reinterpret_cast<const DRAWITEMSTRUCT*> (lp))) return TRUE;
            break;
        case WM_KEYDOWN:
            if (v && wp == VK_SPACE) { forwardSpaceToHost (v, lp); return 0; }
            if (v && (wp == VK_RETURN || wp == VK_ESCAPE)) { v->setCollapsed (false); return 0; }
            break;
        case WM_ERASEBKGND:
            return 1;
    }
    return ::DefWindowProcW (hwnd, msg, wp, lp);
}

LRESULT CALLBACK waveProc (HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    WinView* v = viewOf (hwnd);

    switch (msg)
    {
        case WM_PRINTCLIENT:
        {
            RECT r {};
            ::GetClientRect (hwnd, &r);
            if (v) v->paint (reinterpret_cast<HDC> (wp), r);
            return 0;
        }
        case WM_PAINT:
        {
            PAINTSTRUCT ps;
            HDC dc = ::BeginPaint (hwnd, &ps);
            if (v)
                v->paint (dc, ps.rcPaint);
            ::EndPaint (hwnd, &ps);
            return 0;
        }
        case WM_LBUTTONDOWN:
            if (v) v->onMouseDown (GET_X_LPARAM (lp), GET_Y_LPARAM (lp),
                                  static_cast<int> (wp & (MK_CONTROL | MK_SHIFT)) | modsNow ());
            return 0;
        case WM_MOUSEMOVE:
            if (v) v->onMouseMove (GET_X_LPARAM (lp), GET_Y_LPARAM (lp),
                                  static_cast<int> (wp & (MK_CONTROL | MK_SHIFT)) | modsNow ());
            return 0;
        case WM_LBUTTONUP:
            if (v) v->onMouseUp (GET_X_LPARAM (lp), GET_Y_LPARAM (lp));
            return 0;
        case WM_CAPTURECHANGED:
        case WM_CANCELMODE:
            if (v) v->onMouseUp (0, 0, true);
            return 0;
        case WM_LBUTTONDBLCLK:
            if (v) v->onDoubleClick ();
            return 0;
        case WM_MOUSEWHEEL:
            if (v)
            {
                const POINT point {GET_X_LPARAM (lp), GET_Y_LPARAM (lp)};
                if (v->onNumericWheel (::WindowFromPoint (point), GET_WHEEL_DELTA_WPARAM (wp),
                                       (GET_KEYSTATE_WPARAM (wp) & MK_SHIFT) != 0,
                                       (GET_KEYSTATE_WPARAM (wp) & MK_CONTROL) != 0)) return 0;
                v->onWheelDelta (GET_WHEEL_DELTA_WPARAM (wp), modsNow (), false);
            }
            return 0;
        case WM_MOUSEHWHEEL:
            // 横向滚轮（鼠标滚轮左右拨 / 触控板左右滑）—— 与纵向一样用来平移视野。
            if (v) v->onWheelDelta (GET_WHEEL_DELTA_WPARAM (wp), modsNow (), true);
            return 0;
        case WM_KEYDOWN:
            if (wp == VK_SPACE && v && v->hwnd ())
            {
                forwardSpaceToHost (v, lp);
                return 0;
            }
            break;
        case WM_KEYUP:
            if (wp == VK_SPACE && v && v->hwnd ())
                return 0;
            break;
        case WM_DROPFILES:
            if (v) v->onDropFiles (reinterpret_cast<HDROP> (wp));
            return 0;
        case WM_ERASEBKGND:
            return 1;
        default:
            break;
    }
    return ::DefWindowProcW (hwnd, msg, wp, lp);
}

LRESULT CALLBACK mainProc (HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_NCCREATE)
    {
        CREATESTRUCTW* cs = reinterpret_cast<CREATESTRUCTW*> (lp);
        ::SetWindowLongPtrW (hwnd, GWLP_USERDATA,
                             reinterpret_cast<LONG_PTR> (cs->lpCreateParams));
        return ::DefWindowProcW (hwnd, msg, wp, lp);
    }

    WinView* v = viewOf (hwnd);
    // 使用指南可见时，这一层由主窗口整块自绘（见 setHelp），
    // 所有控件都已经被隐藏，所以下面这些判断不会有「盖住控件」的副作用。
    const bool help  = (v && v->helpVisible ());
    // 拍号设置弹层同理：控件已被藏起来，主窗口整块自绘（理由同 setHelp）。
    const bool popup = (v && v->tsPopupVisible ());

    switch (msg)
    {
        case WM_KEYDOWN:
            if (help && wp == VK_ESCAPE)
            {
                v->hideHelp ();
                return 0;
            }
            if (wp == VK_SPACE && v && v->hwnd ())
            {
                forwardSpaceToHost (v, lp);
                return 0;
            }
            break;
        case WM_KEYUP:
            if (wp == VK_SPACE && v && v->hwnd ())
                return 0;
            break;
        case WM_COMMAND:
            if (v) v->onCommand (LOWORD (wp), HIWORD (wp));
            return 0;
        case WM_TIMER:
            if (wp == IDC_TIMER_UI && v) v->onTimer ();
            return 0;
        case WM_PAINT:
        {
            PAINTSTRUCT ps;
            HDC dc = ::BeginPaint (hwnd, &ps);
            if (popup)
                v->paintTimeSigPopup (dc);
            else if (help)
                v->paintHelp (dc);
            else if (v)
                v->paintChrome (dc);
            ::EndPaint (hwnd, &ps);
            return 0;
        }
        case WM_PRINTCLIENT:
            if (v && wp)
            {
                HDC dc = reinterpret_cast<HDC> (wp);
                if (popup) v->paintTimeSigPopup (dc);
                else if (help) v->paintHelp (dc);
                else v->paintChrome (dc);
            }
            return 0;
        case WM_HSCROLL:
            if (v) v->onHScroll (reinterpret_cast<HWND> (lp));
            return 0;
        case WM_NOTIFY:
            if (v) return v->onNotify (reinterpret_cast<NMHDR*> (lp));
            break;
        case WM_MEASUREITEM:
        {
            auto* item = reinterpret_cast<MEASUREITEMSTRUCT*> (lp);
            if (item && item->CtlID == IDC_TIMESIG_COMBO)
            {
                item->itemHeight = scaledOf (28);
                return TRUE;
            }
            break;
        }
        case WM_CTLCOLORSTATIC:
            if (v)
            {
                HBRUSH b = v->onCtlColor (reinterpret_cast<HDC> (wp),
                                          reinterpret_cast<HWND> (lp));
                if (b)
                    return reinterpret_cast<LRESULT> (b);
            }
            break;
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORLISTBOX:
        case WM_CTLCOLORBTN:
            if (v)
            {
                HDC dc = reinterpret_cast<HDC> (wp);
                HWND ctl = reinterpret_cast<HWND> (lp);
                HBRUSH b = v->onCtlColor (dc, ctl);
                if (b)
                    return reinterpret_cast<LRESULT> (b);
            }
            break;
        case WM_DRAWITEM:
            if (v && v->drawItem (reinterpret_cast<const DRAWITEMSTRUCT*> (lp)))
                return TRUE;
            break;
        case WM_ERASEBKGND:
            return 1;
        case WM_LBUTTONDOWN:
        case WM_RBUTTONDOWN:
        case WM_MBUTTONDOWN:
            // 拍号设置弹层：只有左键点按钮/面板外才有意义，右键与中键一律吞掉。
            if (popup)
            {
                if (msg == WM_LBUTTONDOWN)
                    v->onTimeSigPopupClick (GET_X_LPARAM (lp), GET_Y_LPARAM (lp));
                return 0;
            }
            if (help)
            {
                const auto l = ui::arrangeGuide (static_cast<int> (std::lround (v->panelW () / g_uiScale)),
                                                static_cast<int> (std::lround (v->panelH () / g_uiScale)));
                const POINT point {GET_X_LPARAM (lp), GET_Y_LPARAM (lp)};
                RECT close {scaledOf (l.close.x), scaledOf (l.close.y),
                            scaledOf (l.close.right ()), scaledOf (l.close.bottom ())};
                if (msg == WM_LBUTTONDOWN && ::PtInRect (&close, point)) v->hideHelp ();
                return 0;
            }
            if (v && msg == WM_LBUTTONDOWN)
                v->onTempoClick (GET_X_LPARAM (lp), GET_Y_LPARAM (lp));
            break;
        case WM_MOUSEWHEEL:
        case WM_MOUSEHWHEEL:
        {
            // ⭐ 滚轮消息在 Windows 上是发给【焦点窗口】的，不是发给光标下的窗口
            //   （除非系统开了「悬停滚动非活动窗口」）。焦点基本都在宿主/别的控件
            //   手里，所以滚轮经常被 BPM 输入框之类的控件先接走 —— 这正是用户报的
            //   「滚轮不左右移动波形」。
            //   对策：在主窗口这一层按【光标位置】命中测试，落在波形区就自己处理。
            //   （没接住的那些会经 DefWindowProc 继续上抛给宿主，不抢它的滚轮。）
            // ⚠️ 指南可见时滚轮【只吞掉、不关闭】：指南是固定的一屏文字，但用户
            //    习惯性会往下滚一滚看看还有没有，一滚就关 = 用户报的「刚进去就没了」。
            //    关闭只认鼠标按下（见下面 WM_LBUTTONDOWN 那组）。
            // ⚠️ 指南 / 拍号弹层可见时滚轮【只吞掉、不关闭】：它们是固定的一屏内容，
            //    但用户习惯性会往下滚一滚看看还有没有，一滚就关 = 用户报的「刚进去就没了」。
            //    关闭只认鼠标按下（见上面 WM_LBUTTONDOWN 那组）。
            if (help || popup)
                return 0;
            if (v)
            {
                const int sx = GET_X_LPARAM (lp);   // 滚轮的 lParam 是屏幕坐标
                const int sy = GET_Y_LPARAM (lp);
                if (msg == WM_MOUSEWHEEL && v->onNumericWheel (
                    ::WindowFromPoint (POINT {sx, sy}), GET_WHEEL_DELTA_WPARAM (wp),
                    (GET_KEYSTATE_WPARAM (wp) & MK_SHIFT) != 0,
                    (GET_KEYSTATE_WPARAM (wp) & MK_CONTROL) != 0)) return 0;
                if (v->waveHitTest (sx, sy))
                {
                    v->onWheelDelta (GET_WHEEL_DELTA_WPARAM (wp), modsNow (),
                                     msg == WM_MOUSEHWHEEL);
                    return 0;
                }
            }
            break;      // 不在波形区上 → 原样交给宿主（它可能要用滚轮滚乐谱）
        }
        case WM_DROPFILES:
            if (v) v->onDropFiles (reinterpret_cast<HDROP> (wp));
            return 0;
        case WM_DPICHANGED:
            // 跨屏拖动 / 系统缩放改动时收到（宿主需为 Per-Monitor Aware）。
            // 子窗口通常收不到这条消息，收到了就按新 DPI 重建字体与布局。
            if (v) v->onDpiChanged ();
            return 0;
        default:
            break;
    }
    return ::DefWindowProcW (hwnd, msg, wp, lp);
}

} // namespace

//------------------------------------------------------------------------------
// PlugView：VST3 侧接口实现
//------------------------------------------------------------------------------
PlugView::PlugView (Backend* backend, BackendResolver resolver)
    : m_backend (backend), m_resolver (resolver)
{
    if (!m_backend && m_resolver)
        m_backend = m_resolver ();
}

tresult PLUGIN_API PlugView::queryInterface (const Steinberg::TUID _iid, void** obj)
{
    if (!obj)
        return Steinberg::kInvalidArgument;
    *obj = nullptr;

    if (Steinberg::FUnknownPrivate::iidEqual (_iid, Steinberg::FUnknown::iid) ||
        Steinberg::FUnknownPrivate::iidEqual (_iid, IPlugView::iid))
    {
        *obj = static_cast<IPlugView*> (this);
        addRef ();
        return Steinberg::kResultOk;
    }
    return Steinberg::kNoInterface;
}

tresult PLUGIN_API PlugView::isPlatformTypeSupported (FIDString type)
{
    if (type && std::strcmp (type, Steinberg::kPlatformTypeHWND) == 0)
        return Steinberg::kResultTrue;
    return Steinberg::kResultFalse;
}

tresult PLUGIN_API PlugView::attached (void* parent, FIDString type)
{
    if (!parent || !type || std::strcmp (type, Steinberg::kPlatformTypeHWND) != 0)
        return Steinberg::kResultFalse;

    if (!m_backend && m_resolver)
        m_backend = m_resolver ();

    // 缩放系数要【在 create 之前】定下来：宿主很可能是先 attached 再 getSize，
    // 这样 getSize 报出去的就是已经放大过的尺寸。
    const double scaleBefore = g_uiScale;
    g_uiScale = uiScaleFor (reinterpret_cast<HWND> (parent));

    WinView* v = new WinView (m_backend);
    if (!v->create (reinterpret_cast<HWND> (parent), preferredPanelW (), preferredPanelH ()))
    {
        delete v;
        return Steinberg::kResultFalse;
    }
    v->setOwner (this);
    m_view = v;

    // 若宿主是在 attached 之前就调过 getSize（那时还不知道真实 DPI，报的是 96 DPI
    // 下的尺寸），这里主动请宿主按新尺寸重新调整一次窗口。
    if (g_uiScale != scaleBefore)
        requestResizeToPreferred ();
    return Steinberg::kResultOk;
}

// 请宿主按【默认尺寸】调整窗口（DPI 变化时用）。
// ⚠️ 不能拿 getSize 的结果去用 —— 它现在返回的是「当前尺寸」，
//    而这里要的是「换了 DPI 之后的默认尺寸」（WinView 内部已经按 m_design 算好了）。
void PlugView::requestResizeToPreferred ()
{
    if (!m_frame)
        return;
    ViewRect r;
    r.left = 0;
    r.top = 0;
    if (m_view)
    {
        WinView* v = static_cast<WinView*> (m_view);
        r.right  = v->panelW ();
        r.bottom = v->panelH ();
    }
    else
    {
        r.right  = preferredPanelW ();
        r.bottom = preferredPanelH ();
    }
    m_frame->resizeView (this, &r);
}

tresult PLUGIN_API PlugView::removed ()
{
    if (m_view)
    {
        WinView* v = static_cast<WinView*> (m_view);
        v->destroy ();
        delete v;
        m_view = nullptr;
    }
    return Steinberg::kResultOk;
}

tresult PLUGIN_API PlugView::onWheel (float distance)
{
    // 宿主代发的滚轮（MuseScore 目前不这么做）。没有修饰键信息，按平移处理。
    if (m_view)
        static_cast<WinView*> (m_view)->onWheelDelta (distance > 0 ? -120 : 120, 0, false);
    return Steinberg::kResultOk;
}

tresult PLUGIN_API PlugView::onKeyDown (char16 key, int16 keyCode, int16 modifiers)
{
    (void) key; (void) keyCode; (void) modifiers;
    return Steinberg::kResultOk;
}

tresult PLUGIN_API PlugView::onKeyUp (char16 key, int16 keyCode, int16 modifiers)
{
    (void) key; (void) keyCode; (void) modifiers;
    return Steinberg::kResultOk;
}

tresult PLUGIN_API PlugView::getSize (ViewRect* size)
{
    if (!size)
        return Steinberg::kInvalidArgument;
    size->left = 0;
    size->top = 0;
    // ⭐ 视图已经建好时【如实汇报当前尺寸】：宿主随时会来问「你现在多大」。
    //   这里若还报「默认尺寸」，宿主会照着旧尺寸折腾窗口，当前宽度就被弹回去。
    if (m_view)
    {
        WinView* v = static_cast<WinView*> (m_view);
        size->right  = v->panelW ();
        size->bottom = v->panelH ();
    }
    else
    {
        size->right  = preferredPanelW ();
        size->bottom = preferredPanelH ();
    }
    return Steinberg::kResultOk;
}

tresult PLUGIN_API PlugView::onSize (ViewRect* newSize)
{
    if (m_view && newSize)
        static_cast<WinView*> (m_view)->resize (newSize->right - newSize->left,
                                                newSize->bottom - newSize->top);
    return Steinberg::kResultOk;
}

tresult PLUGIN_API PlugView::onFocus (Steinberg::TBool state)
{
    (void) state;
    return Steinberg::kResultOk;
}

tresult PLUGIN_API PlugView::setFrame (IPlugFrame* frame)
{
    m_frame = frame;
    return Steinberg::kResultOk;
}

// 按 VST3 规范回答「本视图支持缩放」（旧版返回 false = 固定尺寸）。
// 波形区随宽度自适应，面板窄到 340 也仍然排得下 —— 见 layoutChildren。
// ⚠️ MuseScore 4.x 忽略本函数且把窗口 min/max 钉成同一个值，所以别据此对外
//    承诺「可以拖窗口边缘」；真正能改尺寸的途径只有 IPlugFrame::resizeView()。
tresult PLUGIN_API PlugView::canResize ()
{
    return Steinberg::kResultTrue;
}

tresult PLUGIN_API PlugView::checkSizeConstraint (ViewRect* rect)
{
    if (rect)
    {
        int w = rect->right - rect->left;
        int h = rect->bottom - rect->top;
        clampPanelSize (w, h);
        rect->right  = rect->left + w;
        rect->bottom = rect->top  + h;
    }
    return Steinberg::kResultOk;
}

} // namespace ap
