#ifndef _SIRIKIATA_IO_UTIL_HH_
#define _SIRIKIATA_IO_UTIL_HH_
#include <unistd.h>
#include <sys/errno.h>
#include <sys/stat.h>
#include <fcntl.h>
namespace Sirikata {
class DecoderReader;
class DecoderWriter;
}
namespace IOUtil {


inline Sirikata::uint32 ReadFull(Sirikata::DecoderReader * reader, void * vdata, Sirikata::uint32 size) {
    using namespace Sirikata;
    unsigned char * data = reinterpret_cast<unsigned char*>(vdata);
    uint32 copied = 0;
    while (copied < size) {
        std::pair<Sirikata::uint32, Sirikata::JpegError> status = reader->Read(data + copied, size - copied);
        copied += status.first;
        if (status.second != JpegError::nil() || status.first == 0) {
            return copied;
        }
    }
    return copied;
}


class FileReader : public Sirikata::DecoderReader {
    int fp;
    unsigned int total_read;
    unsigned int max_read;
public:
    FileReader(int ff, int max_read_allowed = 0) {
        fp = ff;
        total_read = 0;
        max_read = max_read_allowed;
    }
    std::pair<Sirikata::uint32, Sirikata::JpegError> Read(Sirikata::uint8*data, unsigned int size) {
        if (max_read && total_read + size > max_read) {
            size = max_read - total_read;
            if (size == 0) {
                return std::pair<Sirikata::uint32,
                                 Sirikata::JpegError>(0, Sirikata::JpegError::errEOF());
            }
        }
        using namespace Sirikata;
        do {
            signed long nread = read(fp, data, size);
            if (nread <= 0) {
                if (errno == EINTR) {
                    continue;
                }
                return std::pair<Sirikata::uint32, JpegError>(0, MakeJpegError("Short read"));
            }
            total_read += nread;
            return std::pair<Sirikata::uint32, JpegError>(nread, JpegError::nil());
        } while(true); // while not EINTR
    }
    size_t length() {
        return total_read;
    }
    size_t getsize() {
        return total_read;
    }
};
class FileWriter : public Sirikata::DecoderWriter {
    int fp;
    int total_written;
    bool close_stream;
public:
    FileWriter(int ff, bool do_close_stream) {
        fp = ff;
        total_written = 0;
        close_stream = do_close_stream;
    }
    void Close() {
        if (close_stream) {
            close(fp); // not always useful (eg during SECCOMP)
        }
        fp = -1;
    }
    std::pair<Sirikata::uint32, Sirikata::JpegError> Write(const Sirikata::uint8*data, unsigned int size) {
        using namespace Sirikata;
        size_t data_written = 0;
        while (data_written < size) {
            signed long nwritten = write(fp, data + data_written, size - data_written);
            if (nwritten <= 0) {
                if (errno == EINTR) {
                    continue;
                }
                return std::pair<Sirikata::uint32, JpegError>(data_written, JpegError::errShortHuffmanData());
            }
            data_written += nwritten;
        }
        total_written += size;
        return std::pair<Sirikata::uint32, JpegError>(size, JpegError::nil());
    }
    size_t getsize() {
        return total_written;
    }
};

SIRIKATA_FUNCTION_EXPORT FileReader * OpenFileOrPipe(const char * filename, int is_pipe, int max_size_read);
SIRIKATA_FUNCTION_EXPORT FileWriter * OpenWriteFileOrPipe(const char * filename, int is_pipe);

}
#endif
