/*
 * Copyright 2016-2021 Arm Limited. All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * This file is part of Tarmac Trace Utilities
 */

#include "libtarmac/disktree.hh"
#include "libtarmac/misc.hh"

#include <windows.h>

#include <sstream>

using std::string;

bool get_file_timestamp(const string &filename, uint64_t *out_timestamp)
{
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (!GetFileAttributesEx(filename.c_str(), GetFileExInfoStandard, &data))
        return false;
    *out_timestamp = ((uint64_t)data.ftLastWriteTime.dwHighDateTime << 16) |
                     data.ftLastWriteTime.dwLowDateTime;
    return true;
}

bool is_interactive()
{
    DWORD ignored_output;
    return GetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), &ignored_output);
}

string get_error_message()
{
    DWORD err = GetLastError();

    std::ostringstream oss;
    oss << "error " << err;

    constexpr size_t MAX_MESSAGE_SIZE = 0x10000;
    char msgtext[MAX_MESSAGE_SIZE];
    if (!FormatMessage(
            (FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS), NULL,
            err, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), msgtext,
            MAX_MESSAGE_SIZE - 1, nullptr)) {
        oss << " (unable to format: FormatMessage returned " << GetLastError()
            << ")";
    } else {
        oss << ": " << msgtext;
    }

    string msg = oss.str();
    while (!msg.empty() && msg[msg.size() - 1] == '\n')
        msg.resize(msg.size() - 1);

    return msg;
}

struct MMapFile::PlatformData {
    HANDLE fh;
    HANDLE mh;
};

MMapFile::MMapFile(const string &filename, bool writable)
    : filename(filename), writable(writable)
{
    pdata = new PlatformData;

    pdata->fh = CreateFile(filename.c_str(),
                           GENERIC_READ | (writable ? GENERIC_WRITE : 0),
                           FILE_SHARE_READ, NULL,
                           (writable ? CREATE_ALWAYS : OPEN_EXISTING), 0, NULL);
    if (pdata->fh == INVALID_HANDLE_VALUE)
        err(1, "%s: CreateFile", filename.c_str());

    LARGE_INTEGER size;
    if (!GetFileSizeEx(pdata->fh, &size))
        err(1, "%s: GetFileSizeEx", filename.c_str());
    next_offset = curr_size = size.QuadPart;

    mapping = nullptr;
    pdata->mh = nullptr;
    map();
}

MMapFile::~MMapFile()
{
    unmap();
    if (writable) {
        LARGE_INTEGER pos;
        pos.QuadPart = next_offset;
        if (!SetFilePointerEx(pdata->fh, pos, NULL, FILE_BEGIN))
            err(1, "%s: SetFilePointerEx (truncating file)", filename.c_str());
        if (!SetEndOfFile(pdata->fh))
            err(1, "%s: SetEndOfFile (truncating file)", filename.c_str());
    }
    CloseHandle(pdata->fh);
    delete pdata;
}

void MMapFile::map()
{
    if (!curr_size)
        return;

    assert(!pdata->mh);
    assert(!mapping);

    size_t mapping_size = curr_size;
    if (writable) {
        // Round the file mapping size to a multiple of a page. I observe
        // that CreateFileMapping tends to give ERROR_NO_SYSTEM_RESOURCES
        // if I don't do this, though there's nothing in the documentation
        // to suggest why.
        mapping_size = (mapping_size + 0xFFFF) & ~0xFFFF;
    }

    pdata->mh = CreateFileMapping(
        pdata->fh, NULL, (writable ? PAGE_READWRITE : PAGE_READONLY),
        (mapping_size >> 16) >> 16, mapping_size & 0xFFFFFFFF, NULL);
    if (!pdata->mh)
        err(1, "%s: CreateFileMapping", filename.c_str());

    mapping = MapViewOfFile(pdata->mh,
                            (writable ? FILE_MAP_ALL_ACCESS : FILE_MAP_READ), 0,
                            0, curr_size);
    if (!mapping)
        err(1, "%s: MapViewOfFile", filename.c_str());
}

void MMapFile::unmap()
{
    if (!curr_size)
        return;

    assert(mapping);
    UnmapViewOfFile(mapping);
    mapping = nullptr;

    assert(pdata->mh);
    CloseHandle(pdata->mh);
    pdata->mh = NULL;
}

off_t MMapFile::alloc(size_t size)
{
    assert(writable);
    if ((size_t)(curr_size - next_offset) < size) {
        off_t new_curr_size = (next_offset + size) * 5 / 4 + 65536;
        assert(new_curr_size >= next_offset);

        unmap();

        LARGE_INTEGER pos;
        pos.QuadPart = new_curr_size;
        if (!SetFilePointerEx(pdata->fh, pos, NULL, FILE_BEGIN))
            err(1, "%s: SetFilePointerEx (extending file)", filename.c_str());
        if (!SetEndOfFile(pdata->fh))
            err(1, "%s: SetEndOfFile (extending file)", filename.c_str());
        curr_size = new_curr_size;

        map();
    }
    off_t ret = next_offset;
    next_offset += size;
    return ret;
}
