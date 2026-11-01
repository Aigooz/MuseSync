#include "scoreinfo.h"

#include <windows.h>
#include <shlwapi.h>
#include <xmllite.h>
#include <wrl/client.h>
#include "miniz.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace ap {
namespace {
using Microsoft::WRL::ComPtr;

std::wstring wide (const std::string& path)
{
    const int size = MultiByteToWideChar (CP_UTF8, 0, path.c_str (), -1, nullptr, 0);
    if (size <= 0) return {};
    std::wstring result (size, 0);
    MultiByteToWideChar (CP_UTF8, 0, path.c_str (), -1, result.data (), size);
    result.pop_back ();
    return result;
}

bool readBytes (const std::wstring& path, std::vector<unsigned char>& bytes)
{
    FILE* file = nullptr;
    if (_wfopen_s (&file, path.c_str (), L"rb") != 0) return false;
    _fseeki64 (file, 0, SEEK_END);
    const auto size = _ftelli64 (file);
    _fseeki64 (file, 0, SEEK_SET);
    if (size <= 0 || size > 128 * 1024 * 1024) { fclose (file); return false; }
    bytes.resize (static_cast<size_t> (size));
    const bool ok = fread (bytes.data (), 1, bytes.size (), file) == bytes.size ();
    fclose (file);
    return ok;
}
} // namespace

bool readScoreInfo (const std::string& path, ScoreInfo& info, std::string& error)
{
    info = {};
    error.clear ();
    std::vector<unsigned char> bytes;
    if (!readBytes (wide (path), bytes))
    {
        error = "Cannot read score (maximum 128 MB).";
        return false;
    }

    if (bytes.size () >= 4 && bytes[0] == 'P' && bytes[1] == 'K')
    {
        mz_zip_archive zip {};
        if (!mz_zip_reader_init_mem (&zip, bytes.data (), bytes.size (), 0))
        {
            error = "Invalid MSCZ archive.";
            return false;
        }
        std::vector<unsigned char> xml;
        // The score is the top-level MSCX, not an excerpt under Excerpts/.
        for (mz_uint i = 0; i < mz_zip_reader_get_num_files (&zip); ++i)
        {
            mz_zip_archive_file_stat stat {};
            if (!mz_zip_reader_file_stat (&zip, i, &stat)) continue;
            const std::string name = stat.m_filename;
            if (name.size () < 5 || name.substr (name.size () - 5) != ".mscx" ||
                name.find ('/') != std::string::npos || name.find ('\\') != std::string::npos ||
                stat.m_uncomp_size > 128 * 1024 * 1024) continue;
            xml.resize (static_cast<size_t> (stat.m_uncomp_size));
            if (!mz_zip_reader_extract_to_mem (&zip, i, xml.data (), xml.size (), 0)) xml.clear ();
            break;
        }
        mz_zip_reader_end (&zip);
        bytes.swap (xml);
    }
    if (bytes.empty ()) { error = "Score XML was not found."; return false; }

    const HRESULT com = CoInitializeEx (nullptr, COINIT_MULTITHREADED);
    struct ComScope { bool release; ~ComScope () { if (release) CoUninitialize (); } } scope {SUCCEEDED (com)};
    ComPtr<IStream> stream;
    stream.Attach (SHCreateMemStream (bytes.data (), static_cast<UINT> (bytes.size ())));
    ComPtr<IXmlReader> reader;
    if (!stream || FAILED (CreateXmlReader (__uuidof (IXmlReader),
                         reinterpret_cast<void**> (reader.GetAddressOf ()), nullptr)) ||
        FAILED (reader->SetInput (stream.Get ())))
    {
        error = "XML reader could not open the score.";
        return false;
    }
    reader->SetProperty (XmlReaderProperty_DtdProcessing, DtdProcessing_Prohibit);
    std::vector<std::wstring> elements;
    bool firstStaff = false, staffDone = false, scoreSeen = false;
    int measure = 0;
    float bpm = 0;
    int beats = 0, denominator = 0;
    XmlNodeType type;
    HRESULT status;
    while (S_OK == (status = reader->Read (&type)))
    {
        const wchar_t* name = nullptr;
        if (type == XmlNodeType_Element)
        {
            reader->GetLocalName (&name, nullptr);
            const std::wstring parent = elements.empty () ? L"" : elements.back ();
            elements.push_back (name ? name : L"");
            if (elements.back () == L"Score") scoreSeen = true;
            if (elements.back () == L"Staff" && parent == L"Score" && !staffDone) firstStaff = true;
            if (firstStaff && elements.back () == L"Measure") ++measure;
            if (reader->IsEmptyElement ()) elements.pop_back ();
        }
        else if (type == XmlNodeType_EndElement)
        {
            if (elements.size () >= 2 && elements.back () == L"Staff" &&
                elements[elements.size () - 2] == L"Score") { firstStaff = false; staffDone = true; }
            if (!elements.empty ()) elements.pop_back ();
        }
        else if ((type == XmlNodeType_Text || type == XmlNodeType_CDATA) && firstStaff &&
                 measure == 1 && elements.size () >= 2)
        {
            const wchar_t* value = nullptr;
            reader->GetValue (&value, nullptr);
            if (!value) continue;
            const auto& leaf = elements.back ();
            const auto& parent = elements[elements.size () - 2];
            if (leaf == L"tempo" && parent == L"Tempo" && bpm == 0)
            {
                const double v = wcstod (value, nullptr) * 60.0;
                if (std::isfinite (v) && v >= 1 && v <= 1000) bpm = static_cast<float> (v);
            }
            if (leaf == L"sigN" && parent == L"TimeSig" && beats == 0) beats = _wtoi (value);
            if (leaf == L"sigD" && parent == L"TimeSig" && denominator == 0) denominator = _wtoi (value);
        }
    }
    if (FAILED (status) || !scoreSeen || measure == 0) { error = "Invalid MuseScore XML."; return false; }
    // MuseScore's implicit opening values when no marking is present.
    info.bpm = bpm > 0 ? bpm : 120.f;
    info.beats = 4;
    if (beats > 0 && beats <= 64 && denominator > 0 && denominator <= 64 &&
        (denominator & (denominator - 1)) == 0)
    {
        info.beats = beats;
        info.denominator = denominator;
    }
    return true;
}
} // namespace ap
