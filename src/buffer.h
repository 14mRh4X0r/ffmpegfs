/*
 * cache class header for ffmpegfs
 *
 * Copyright (C) 2017-2018 Norbert Schlia (nschlia@oblivion-software.de)
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#ifndef BUFFER_H
#define BUFFER_H

#pragma once

#include "fileio.h"

#define CACHE_CHECK_BIT(mask, var)  ((mask) == (mask & (var)))

#define CLOSE_CACHE_NOOPT   0x00                        // Dummy, do nothing special
#define CLOSE_CACHE_FREE    0x01                        // Free memory for cache entry
#define CLOSE_CACHE_DELETE  (0x02 | CLOSE_CACHE_FREE)   // Delete cache entry, will unlink cached file! Implies CLOSE_CACHE_FREE.

using namespace std;

class Buffer : public fileio
{
public:
    explicit Buffer();
    virtual ~Buffer();

    virtual VIRTUALTYPE type() const;

    bool init(bool erase_cache = false);                // Initialise cache, if erase_cache = true delete old file before opening
    bool release(int flags = CLOSE_CACHE_NOOPT);

    virtual int bufsize() const;
    virtual int open(LPCVIRTUALFILE virtualfile);
    virtual int open(const string & filename);
    virtual int read(void *data, int maxlen);
    virtual int error() const;
    virtual int duration() const;
    virtual size_t size() const;
    virtual size_t tell() const;
    virtual int seek(long offset, int whence);
    virtual bool eof() const;
    virtual void close();

    size_t write(const uint8_t* data, size_t length);
    bool flush();
    bool clear();
    bool reserve(size_t size);
    size_t buffer_watermark() const;
    bool copy(uint8_t* out_data, size_t offset, size_t bufsize);

    void lock();
    void unlock();

    const string & filename() const;
    const string & cachefile() const;

    static const string & make_cachefile_name(string &cachefile, const string & filename);
    static bool remove_file(const string & filename);

protected:
    bool remove_cachefile();

private:
    uint8_t* write_prepare(size_t length);
    void increment_pos(ptrdiff_t increment);
    bool reallocate(size_t newsize);

private:
    pthread_mutex_t m_mutex;
    string          m_filename;
    string          m_cachefile;
    size_t          m_buffer_pos;           // Read/write position
    size_t          m_buffer_watermark;     // Number of bytes in buffer
    volatile bool   m_is_open;
    size_t          m_buffer_size;          // Current buffer size
    uint8_t *       m_buffer;
    int             m_fd;
};

#endif
