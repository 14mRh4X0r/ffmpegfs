/*
 * Copyright (C) 2017-2020 Norbert Schlia (nschlia@oblivion-software.de)
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
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * On Debian systems, the complete text of the GNU General Public License
 * Version 3 can be found in `/usr/share/common-licenses/GPL-3'.
 */

/**
 * @file
 * @brief Buffer class implementation
 *
 * @ingroup ffmpegfs
 *
 * @author Norbert Schlia (nschlia@oblivion-software.de)
 * @copyright Copyright (C) 2017-2020 Norbert Schlia (nschlia@oblivion-software.de)
 */

#include "buffer.h"
#include "ffmpegfs.h"
#include "ffmpeg_utils.h"
#include "logging.h"

#include <unistd.h>
#include <sys/mman.h>
#include <libgen.h>

// Initially Buffer is empty. It will be allocated as needed.
Buffer::Buffer()
    : m_fd(-1)
    , m_buffer(nullptr)
    , m_buffer_pos(0)
    , m_buffer_watermark(0)
    , m_buffer_size(0)
{
}

// If buffer_data was never allocated, this is a no-op.
Buffer::~Buffer()
{
    release();
}

VIRTUALTYPE Buffer::type() const
{
    return VIRTUALTYPE_BUFFER;
}

size_t Buffer::bufsize() const
{
    return 0;   // Not applicable
}

int Buffer::open(LPVIRTUALFILE virtualfile)
{
    if (virtualfile == nullptr)
    {
        errno = EINVAL;
        return (EOF);
    }

    set_virtualfile(virtualfile);

    return 0;
}

bool Buffer::init(bool erase_cache)
{
    std::lock_guard<std::recursive_mutex> lck (m_mutex);

    if (is_open())
    {
        return true;
    }

    bool success = true;

    make_cachefile_name(m_cachefile, filename(), params.current_format(virtualfile())->fileext());

    try
    {
        // Create the path to the cache file
        char *cachefile = new_strdup(m_cachefile);

        if (cachefile == nullptr)
        {
            Logging::error(m_cachefile, "Error opening cache file: Out of memory");
            errno = ENOMEM;
            throw false;
        }

        if (mktree(dirname(cachefile), S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH) && errno != EEXIST)
        {
            Logging::error(m_cachefile, "Error creating cache directory: (%1) %2", errno, strerror(errno));
            delete [] cachefile;
            throw false;
        }
        errno = 0;  // reset EEXIST, error can safely be ignored here

        delete [] cachefile;

        m_fd                = -1;
        m_buffer            = nullptr;
        m_buffer_pos        = 0;
        m_buffer_watermark  = 0;
        m_buffer_size       = 0;

        if (erase_cache)
        {
            remove_cachefile();
            errno = 0;  // ignore this error
        }

        size_t filesize     = 0;
        bool isdefaultsize  = false;
        uint8_t *p          = nullptr;

        if (!map_file(m_cachefile, &m_fd, &p, &filesize, &isdefaultsize, 0))
        {
            throw false;
        }

        if (!isdefaultsize)
        {
            m_buffer_pos = m_buffer_watermark = filesize;
        }

        m_buffer_size       = filesize;
        m_buffer            = static_cast<uint8_t*>(p);
    }
    catch (bool _success)
    {
        success = _success;

        if (!success)
        {
            m_fd                = -1;
            m_buffer            = nullptr;
            m_buffer_pos        = 0;
            m_buffer_watermark  = 0;
            m_buffer_size       = 0;
        }
    }

    return success;
}

bool Buffer::map_file(const std::string & filename, int *fd, uint8_t **p, size_t *filesize, bool *isdefaultsize, off_t defaultsize) const
{
    bool success = true;

    Logging::trace(filename, "Mapping cache file.");

    try
    {
        struct stat sb;

        *fd = ::open(filename.c_str(), O_CREAT | O_RDWR, static_cast<mode_t>(0644));
        if (*fd == -1)
        {
            Logging::error(filename, "Error opening cache file: (%1) %2", errno, strerror(errno));
            throw false;
        }

        if (fstat(*fd, &sb) == -1)
        {
            Logging::error(filename, "File stat failed: (%1) %2 (fd = %3)", errno, strerror(errno), *fd);
            throw false;
        }

        if (!S_ISREG(sb.st_mode))
        {
            Logging::error(filename, "Not a file.");
            throw false;
        }

        if (!sb.st_size || *isdefaultsize)
        {
            // If file is empty or did not exist set file size to default

            if (!defaultsize)
            {
                defaultsize = sysconf(_SC_PAGESIZE);
            }

            if (ftruncate(*fd, defaultsize) == -1)
            {
                Logging::error(filename, "Error calling ftruncate() to 'stretch' the file: (%1) %2 (fd = %3)", errno, strerror(errno), *fd);
                throw false;
            }

            *filesize       = static_cast<size_t>(defaultsize);
            *isdefaultsize  = true;
        }
        else
        {
            // Keep size
            *filesize       = static_cast<size_t>(sb.st_size);
            *isdefaultsize  = false;
        }

        *p = static_cast<uint8_t *>(mmap(nullptr, *filesize, PROT_READ | PROT_WRITE, MAP_SHARED, *fd, 0));
        if (*p == MAP_FAILED)
        {
            Logging::error(filename, "File mapping failed: (%1) %2 (fd = %3)", errno, strerror(errno), *fd);
            throw false;
        }
    }
    catch (bool _success)
    {
        success = _success;

        if (!success && *fd != -1)
        {
            ::close(*fd);
            *fd = -1;
        }
    }

    return success;
}

bool Buffer::unmap_file(const std::string &filename, int * fd, uint8_t **p, size_t * filesize, size_t *buffer_pos) const
{
    bool success = true;

    Logging::trace(filename, "Unmapping cache file.");

    void *  __p         = *p;
    size_t  __filesize  = *filesize;
    int     __fd        = *fd;

    // Clear all variables
    p           = nullptr;
    *filesize   = 0;
    if (buffer_pos != nullptr)
    {
        *buffer_pos = 0;
    }
    *fd         = -1;

    if (__p != nullptr)
    {
        if (munmap(__p, __filesize ? __filesize : static_cast<size_t>(sysconf(_SC_PAGESIZE))) == -1) // Make sure we do not unmap a zero size file (spitzs EINVBAL error)
        {
            Logging::error(filename, "Unmapping cache file failed: (%1) %2 %3", errno, strerror(errno), __filesize);
            fprintf(stderr, "P = %p size = %zi\n", __p, __filesize);
            success = false;
        }
    }

    if (__fd != -1)
    {
        if (__filesize)
        {
            if (ftruncate(__fd, static_cast<off_t>(__filesize)) == -1)
            {
                Logging::error(filename, "Error calling ftruncate() to resize and close the cache file: (%1) %2 (fd = %3)", errno, strerror(errno), __fd);
                success = false;
            }
            ::close(__fd);
        }
        else
        {
            ::close(__fd);

            if (unlink(filename.c_str()))
            {
                Logging::error(filename, "Error removing the cache file: (%1) %2 (fd = %3)", errno, strerror(errno), __fd);
                success = false;
            }
        }
    }

    return success;
}

bool Buffer::release(int flags /*= CACHE_CLOSE_NOOPT*/)
{
    std::lock_guard<std::recursive_mutex> lck (m_mutex);

    bool success = true;

    if (!is_open())
    {
        if (CACHE_CHECK_BIT(CACHE_CLOSE_DELETE, flags))
        {
            remove_cachefile();
            errno = 0;  // ignore this error
        }

        return true;
    }

    // Write it now to disk
    flush();

    if (!unmap_file(m_cachefile, &m_fd, &m_buffer, &m_buffer_watermark, &m_buffer_pos))
    {
        success = false;
    }

    if (CACHE_CHECK_BIT(CACHE_CLOSE_DELETE, flags))
    {
        remove_cachefile();
        errno = 0;  // ignore this error
    }

    return success;
}

bool Buffer::remove_cachefile()
{
    return remove_file(m_cachefile);
}

bool Buffer::flush()
{
    std::lock_guard<std::recursive_mutex> lck (m_mutex);

    if (m_buffer != nullptr)
    {
        if (msync(m_buffer, m_buffer_size, MS_SYNC) == -1)
        {
            Logging::error(m_cachefile, "Could not sync to disk: (%1) %2", errno, strerror(errno));
            return false;
        }
    }
    else
    {
        errno = EPERM;
        return false;
    }

    return true;
}

bool Buffer::clear()
{
    std::lock_guard<std::recursive_mutex> lck (m_mutex);

    if (m_buffer == nullptr)
    {
        errno = EBADF;
        return false;
    }

    bool success = true;

    m_buffer_pos        = 0;
    m_buffer_watermark  = 0;
    m_buffer_size       = 0;

    // If empty set file size to 1 page
    long filesize = sysconf (_SC_PAGESIZE);

    if (m_fd != -1 && ftruncate(m_fd, filesize) == -1)
    {
        Logging::error(m_cachefile, "Error calling ftruncate() to clear the file: (%1) %2 (fd = %3)", errno, strerror(errno), m_fd);
        success = false;
    }

    return success;
}

bool Buffer::reserve(size_t size)
{
    std::lock_guard<std::recursive_mutex> lck (m_mutex);

    if (m_buffer == nullptr)
    {
        errno = EBADF;
        return false;
    }

    bool success = true;

    if (!size)
    {
        size = m_buffer_size;
    }

    m_buffer = static_cast<uint8_t*>(mremap(m_buffer, m_buffer_size, size, MREMAP_MAYMOVE));
    if (m_buffer != nullptr)
    {
        m_buffer_size = size;
    }

    if (ftruncate(m_fd, static_cast<off_t>(m_buffer_size)) == -1)
    {
        Logging::error(m_cachefile, "Error calling ftruncate() to resize the file: (%1) %2 (fd = %3)", errno, strerror(errno), m_fd);
        success = false;
    }

    return ((m_buffer != nullptr) && success);
}

size_t Buffer::write(const uint8_t* data, size_t length)
{
    std::lock_guard<std::recursive_mutex> lck (m_mutex);

    if (m_buffer == nullptr)
    {
        errno = EBADF;
        return 0;
    }

    uint8_t* write_ptr = write_prepare(length);
    if (!write_ptr)
    {
        length = 0;
    }
    else
    {
        memcpy(write_ptr, data, length);
        increment_pos(length);
    }

    return length;
}

uint8_t* Buffer::write_prepare(size_t length)
{
    if (reallocate(m_buffer_pos + length))
    {
        if (m_buffer_watermark < m_buffer_pos + length)
        {
            m_buffer_watermark = m_buffer_pos + length;
        }
        return m_buffer + m_buffer_pos;
    }
    else
    {
        errno = ESPIPE;
        return nullptr;
    }
}

void Buffer::increment_pos(size_t increment)
{
    m_buffer_pos += increment;
}

int Buffer::seek(int64_t offset, int whence)
{
    if (m_buffer == nullptr)
    {
        errno = EBADF;
        return (EOF);
    }

    off_t seek_pos;

    switch (whence)
    {
    case SEEK_SET:
    {
        seek_pos = offset;
        break;
    }
    case SEEK_CUR:
    {
        seek_pos = static_cast<off_t>(tell()) + offset;
        break;
    }
    case SEEK_END:
    {
        seek_pos = static_cast<off_t>(size()) + offset;
        break;
    }
    default:
    {
        errno = EINVAL;
        return (EOF);
    }
    }

    if (seek_pos > static_cast<off_t>(size()))
    {
        m_buffer_pos = size();  // Cannot go beyond EOF. Set position to end, leave errno untouched.
        return 0;
    }

    if (seek_pos < 0)           // Cannot go before head, leave position untouched, set errno.
    {
        errno = EINVAL;
        return (EOF);
    }

    m_buffer_pos = static_cast<size_t>(seek_pos);
    return 0;
}

size_t Buffer::tell() const
{
    return m_buffer_pos;
}

int64_t Buffer::duration() const
{
    return AV_NOPTS_VALUE;  // not applicable
}

size_t Buffer::size() const
{
    return m_buffer_size;
}

size_t Buffer::buffer_watermark() const
{
    return m_buffer_watermark;
}

bool Buffer::copy(std::vector<uint8_t> * out_data, size_t offset)
{
    return copy(out_data->data(), offset, out_data->size());
}

bool Buffer::copy(uint8_t* out_data, size_t offset, size_t bufsize)
{
    std::lock_guard<std::recursive_mutex> lck (m_mutex);

    if (m_buffer == nullptr)
    {
        errno = EBADF;
        return false;
    }

    bool success = true;

    if (size() >= offset)
    {
        if (size() < offset + bufsize)
        {
            bufsize = size() - offset - 1;
        }

        memcpy(out_data, m_buffer + offset, bufsize);
    }
    else
    {
        errno = ESPIPE; // Changed from ENOMEM because this was misleading.
        success = false;
    }

    return success;
}

bool Buffer::reallocate(size_t newsize)
{
    if (newsize > size())
    {
        size_t oldsize = size();

        if (!reserve(newsize))
        {
            return false;
        }

        Logging::trace(filename(), "Buffer reallocate: %1 -> %2.", oldsize, newsize);
    }
    return true;
}

const std::string & Buffer::cachefile() const
{
    return m_cachefile;
}

const std::string & Buffer::make_cachefile_name(std::string & cachefile, const std::string & filename, const std::string & fileext)
{
    transcoder_cache_path(cachefile);

    cachefile += params.m_mountpath;
    cachefile += filename;
    cachefile += ".cache.";
    cachefile += fileext;

    return cachefile;
}

bool Buffer::remove_file(const std::string & filename)
{
    if (unlink(filename.c_str()) && errno != ENOENT)
    {
        Logging::warning(filename, "Cannot unlink the file: (%1) %2", errno, strerror(errno));
        return false;
    }
    else
    {
        errno = 0;
        return true;
    }
}

size_t Buffer::read(void * /*data*/, size_t /*size*/)
{
    // Not implemented
    errno = EPERM;
    return 0;
}

int Buffer::error() const
{
    return errno;
}

bool Buffer::eof() const
{
    return (tell() == size());
}

void Buffer::close()
{
    release();
}

bool Buffer::is_open() const
{
    return (m_fd != -1 && (fcntl(m_fd, F_GETFL) != -1 || errno != EBADF));
}
