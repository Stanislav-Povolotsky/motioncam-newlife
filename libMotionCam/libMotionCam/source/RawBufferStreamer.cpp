#include "motioncam/RawBufferStreamer.h"
#include "motioncam/RawContainer.h"
#include "motioncam/RawBufferManager.h"
#include "motioncam/Logger.h"
#include "motioncam/Util.h"
#include "motioncam/Measure.h"
#include "motioncam/AudioInterface.h"

#include <zstd.h>
#include <zstd_errors.h>
#include <tinywav.h>
#include <memory>
#include <vint.h>
#include <vp4.h>
#include <bitpack.h>

#if defined(__APPLE__) || defined(__ANDROID__) || defined(__linux__)
    #include <unistd.h>
    #include <arpa/inet.h>
#elif defined(_WIN32)
    #include <WinSock2.h>
#endif

namespace motioncam {
    const int NumProcessThreads  = 2;

    const int SoundSampleRateHz       = 48000;
    const int SoundChannelCount       = 1;

#if defined(__APPLE__) || defined(__ANDROID__) || defined(__linux__)
    static inline __attribute__((always_inline))
#endif
    uint16_t RAW10(uint8_t* data, int16_t x, int16_t y, const uint16_t stride) {
        const uint16_t X = (x >> 2) << 2;
        const uint32_t xoffset = (y * stride) + ((10*X) >> 3);
        
        uint16_t p = x - X;
        uint8_t shift = p << 1;
        
        return (((uint16_t) data[xoffset + p]) << 2) | ((((uint16_t) data[xoffset + 4]) >> shift) & 0x03);
    }

#if defined(__APPLE__) || defined(__ANDROID__) || defined(__linux__)
    static inline __attribute__((always_inline))
#endif
    uint16_t RAW12(uint8_t* data, int16_t x, int16_t y, const uint16_t stride) {
        const uint16_t X = (x >> 1) << 1;
        const uint32_t xoffset = (y * stride) + ((12*X) >> 3);
        
        uint16_t p = x - X;
        uint8_t shift = p << 2;
        
        return (((uint16_t) data[xoffset + p]) << 4) | ((((uint16_t) data[xoffset + 2]) >> shift) & 0x0F);
    }

#if defined(__APPLE__) || defined(__ANDROID__) || defined(__linux__)
    static inline __attribute__((always_inline))
#endif
    uint16_t RAW16(uint8_t* data, int16_t x, int16_t y, const uint16_t stride) {
        const uint32_t offset = (y*stride) + (x*2);
        
        return ((uint16_t) data[offset]) | (((uint16_t)data[offset+1]) << 8);
    }

    RawBufferStreamer::RawBufferStreamer() :
        mRunning(false),
        mAudioFd(-1),
        mCropHeight(0),
        mCropWidth(0),
        mBin(false)
    {
    }

    RawBufferStreamer::~RawBufferStreamer() {
        stop();
    }

    void RawBufferStreamer::start(const std::vector<int>& fds,
                                  const int& audioFd,
                                  const std::shared_ptr<AudioInterface> audioInterface,
                                  const RawCameraMetadata& cameraMetadata) {
        stop();
        
        if(fds.empty()) {
            logger::log("No file descriptors found");
            return;
        }
        
        mRunning = true;
        mWrittenFrames = 0;
        mWrittenBytes = 0;
        mAcceptedFrames = 0;
        
        // Start audio interface
        if(audioInterface && audioFd >= 0) {
            mAudioInterface = audioInterface;
            mAudioFd = audioFd;
            
            mAudioInterface->start(SoundSampleRateHz, SoundChannelCount);
        }

        mStartTime = std::chrono::steady_clock::now();
        
        // Create IO threads with maximum priority
        for(int i = 0; i < fds.size(); i++) {
            auto ioThread = std::unique_ptr<std::thread>(new std::thread(&RawBufferStreamer::doStream, this, fds[i], cameraMetadata, (int)fds.size()));

        #if defined(__APPLE__) || defined(__ANDROID__) || defined(__linux__)
            // Update priority
            sched_param priority{};
            priority.sched_priority = 99;

            pthread_setschedparam(ioThread->native_handle(), SCHED_FIFO, &priority);
        #endif

            mIoThreads.push_back(std::move(ioThread));
        }
                
        // Create process threads
        for(int i = 0; i < NumProcessThreads; i++) {
            auto t = std::unique_ptr<std::thread>(new std::thread(&RawBufferStreamer::doProcess, this));
            
            mProcessThreads.push_back(std::move(t));
        }
    }

    void RawBufferStreamer::add(const std::shared_ptr<RawImageBuffer>& frame) {
        mUnprocessedBuffers.enqueue(frame);
        mAcceptedFrames++;
    }

    void RawBufferStreamer::stop() {
        mRunning = false;

        if(mAudioInterface) {
            mAudioInterface->stop();
            
            // Flush to disk
            uint32_t numFrames = 0;
            auto& buffer = mAudioInterface->getAudioData(numFrames);

            FILE* audioFile = fdopen(mAudioFd, "w");
            if(audioFile != NULL) {
                TinyWav tw = {0};

                if(tinywav_open_write_f(
                    &tw,
                    mAudioInterface->getChannels(),
                    mAudioInterface->getSampleRate(),
                    TW_INT16,
                    TW_INTERLEAVED,
                    audioFile) == 0)
                {
                    tinywav_write_f(&tw, (void*)buffer.data(), (int)(numFrames));
                }
                
                tinywav_close_write(&tw);
            }
            else {
                #if defined(__APPLE__) || defined(__ANDROID__) || defined(__linux__)
                    close(mAudioFd);
                #endif
            }
        }
        
        mAudioInterface = nullptr;
        mAudioFd = -1;

        for(auto& thread : mProcessThreads) {
            thread->join();
        }
        
        mProcessThreads.clear();

        for(auto& thread : mIoThreads) {
            thread->join();
        }
        
        mIoThreads.clear();
    }

    void RawBufferStreamer::setCropAmount(int width, int height) {
        // Only allow cropping when not running
        if(!mRunning) {
            mCropHeight = height;
            mCropWidth = width;
        }
    }

    void RawBufferStreamer::setBin(bool bin) {
        mBin = bin;
    }

    size_t RawBufferStreamer::cropAndBin_RAW10(RawImageBuffer& buffer,
                                               uint8_t* data,
                                               const int16_t ystart,
                                               const int16_t yend,
                                               const int16_t xstart,
                                               const int16_t xend,
                                               const int16_t binnedWidth) const
    {
        std::vector<uint16_t> row0(binnedWidth);
        std::vector<uint16_t> row1(binnedWidth);
        size_t offset = 0;

        auto* encodeFunc = &bitnzpack128v16;

        for(int16_t y = ystart; y < yend; y+=4) {
            for(int16_t x = xstart; x < xend; x+=4) {
                uint16_t iy = y;

                // Unroll inner loops
                
                {
                    const int16_t ix = x;
                    
                    const int16_t ix_m2 = (std::max)(0, ix - 2);
                    const int16_t ix_p2 = (ix + 2) % buffer.width;
                    
                    const int16_t iy_m2 = (std::max)(0, iy - 2);
                    const int16_t iy_p2 = (iy + 2) % buffer.height;
                    
                    const uint16_t p0 = RAW10(data, ix_m2, iy_m2,  buffer.rowStride);
                    const uint16_t p1 = RAW10(data, ix,    iy_m2,  buffer.rowStride) << 1;
                    const uint16_t p2 = RAW10(data, ix_p2, iy_m2,  buffer.rowStride);
                    
                    const uint16_t p3 = RAW10(data, ix_m2, iy,     buffer.rowStride) << 1;
                    const uint16_t p4 = RAW10(data, ix,    iy,     buffer.rowStride) << 2;
                    const uint16_t p5 = RAW10(data, ix_p2, iy,     buffer.rowStride) << 1;
                    
                    const uint16_t p6 = RAW10(data, ix_m2, iy_p2,  buffer.rowStride);
                    const uint16_t p7 = RAW10(data, ix,    iy_p2,  buffer.rowStride) << 1;
                    const uint16_t p8 = RAW10(data, ix_p2, iy_p2,  buffer.rowStride);

                    const uint16_t out = (p0 + p1 + p2 + p3 + p4 + p5 + p6 + p7 + p8) >> 4;
                    
                    const uint16_t X = ((x - xstart) >> 2);
                    row0[X] = out;
                }

                {
                    const uint16_t ix = x + 1;

                    const int16_t ix_m2 = (std::max)(0, ix - 2);
                    const int16_t ix_p2 = (ix + 2) % buffer.width;
                    
                    const int16_t iy_m2 = (std::max)(0, iy - 2);
                    const int16_t iy_p2 = (iy + 2) % buffer.height;

                    const uint16_t p0 = RAW10(data, ix_m2, iy_m2,  buffer.rowStride);
                    const uint16_t p1 = RAW10(data, ix,    iy_m2,  buffer.rowStride) << 1;
                    const uint16_t p2 = RAW10(data, ix_p2, iy_m2,  buffer.rowStride);
                    
                    const uint16_t p3 = RAW10(data, ix_m2, iy,     buffer.rowStride) << 1;
                    const uint16_t p4 = RAW10(data, ix,    iy,     buffer.rowStride) << 2;
                    const uint16_t p5 = RAW10(data, ix_p2, iy,     buffer.rowStride) << 1;
                    
                    const uint16_t p6 = RAW10(data, ix_m2, iy_p2,  buffer.rowStride);
                    const uint16_t p7 = RAW10(data, ix,    iy_p2,  buffer.rowStride) << 1;
                    const uint16_t p8 = RAW10(data, ix_p2, iy_p2,  buffer.rowStride);

                    const uint16_t out = (p0 + p1 + p2 + p3 + p4 + p5 + p6 + p7 + p8) >> 4;

                    const uint16_t X = ((x - xstart) >> 2) + (binnedWidth >> 1);
                    row0[X] = out;
                }

                // Next row
                iy = y + 1;

                {
                    const uint16_t ix = x;
                    
                    const int16_t ix_m2 = (std::max)(0, ix - 2);
                    const int16_t ix_p2 = (ix + 2) % buffer.width;
                    
                    const int16_t iy_m2 = (std::max)(0, iy - 2);
                    const int16_t iy_p2 = (iy + 2) % buffer.height;

                    const uint16_t p0 = RAW10(data, ix_m2, iy_m2,  buffer.rowStride);
                    const uint16_t p1 = RAW10(data, ix,    iy_m2,  buffer.rowStride) << 1;
                    const uint16_t p2 = RAW10(data, ix_p2, iy_m2,  buffer.rowStride);
                    
                    const uint16_t p3 = RAW10(data, ix_m2, iy,     buffer.rowStride) << 1;
                    const uint16_t p4 = RAW10(data, ix,    iy,     buffer.rowStride) << 2;
                    const uint16_t p5 = RAW10(data, ix_p2, iy,     buffer.rowStride) << 1;
                    
                    const uint16_t p6 = RAW10(data, ix_m2, iy_p2,  buffer.rowStride);
                    const uint16_t p7 = RAW10(data, ix,    iy_p2,  buffer.rowStride) << 1;
                    const uint16_t p8 = RAW10(data, ix_p2, iy_p2,  buffer.rowStride);

                    const uint16_t out = (p0 + p1 + p2 + p3 + p4 + p5 + p6 + p7 + p8) >> 4;

                    const uint16_t X = ((x - xstart) >> 2);
                    row1[X] = out;
                }
                
                {
                    const uint16_t ix = x + 1;
                    
                    const int16_t ix_m2 = (std::max)(0, ix - 2);
                    const int16_t ix_p2 = (ix + 2) % buffer.width;
                    
                    const int16_t iy_m2 = (std::max)(0, iy - 2);
                    const int16_t iy_p2 = (iy + 2) % buffer.height;

                    const uint16_t p0 = RAW10(data, ix_m2, iy_m2,  buffer.rowStride);
                    const uint16_t p1 = RAW10(data, ix,    iy_m2,  buffer.rowStride) << 1;
                    const uint16_t p2 = RAW10(data, ix_p2, iy_m2,  buffer.rowStride);
                    
                    const uint16_t p3 = RAW10(data, ix_m2, iy,     buffer.rowStride) << 1;
                    const uint16_t p4 = RAW10(data, ix,    iy,     buffer.rowStride) << 2;
                    const uint16_t p5 = RAW10(data, ix_p2, iy,     buffer.rowStride) << 1;
                    
                    const uint16_t p6 = RAW10(data, ix_m2, iy_p2,  buffer.rowStride);
                    const uint16_t p7 = RAW10(data, ix,    iy_p2,  buffer.rowStride) << 1;
                    const uint16_t p8 = RAW10(data, ix_p2, iy_p2,  buffer.rowStride);

                    const uint16_t out = (p0 + p1 + p2 + p3 + p4 + p5 + p6 + p7 + p8) >> 4;

                    const uint16_t X = ((x - xstart) >> 2) + (binnedWidth >> 1);
                    row1[X] = out;
                }
            }
            
            size_t writtenBytes = encodeFunc(row0.data(), row0.size(), data+offset);
            offset += writtenBytes;
            
            writtenBytes = encodeFunc(row1.data(), row1.size(), data+offset);
            offset += writtenBytes;
        }
        
        return offset;
    }

    size_t RawBufferStreamer::cropAndBin_RAW12(RawImageBuffer& buffer,
                                               uint8_t* data,
                                               const int16_t ystart,
                                               const int16_t yend,
                                               const int16_t xstart,
                                               const int16_t xend,
                                               const int16_t binnedWidth) const
    {
        std::vector<uint16_t> row0(binnedWidth);
        std::vector<uint16_t> row1(binnedWidth);
        uint32_t offset = 0;
        auto* encodeFunc = &bitnzpack128v16;

        for(int16_t y = ystart; y < yend; y+=4) {
            for(int16_t x = xstart; x < xend; x+=4) {
                uint16_t iy = y;

                // Unroll inner loops
                
                {
                    const int16_t ix = x;
                    
                    const int16_t ix_m2 = (std::max)(0, ix - 2);
                    const int16_t ix_p2 = (ix + 2) % buffer.width;
                    
                    const int16_t iy_m2 = (std::max)(0, iy - 2);
                    const int16_t iy_p2 = (iy + 2) % buffer.height;
                    
                    const uint16_t p0 = RAW12(data, ix_m2, iy_m2,  buffer.rowStride);
                    const uint16_t p1 = RAW12(data, ix,    iy_m2,  buffer.rowStride) << 1;
                    const uint16_t p2 = RAW12(data, ix_p2, iy_m2,  buffer.rowStride);
                    
                    const uint16_t p3 = RAW12(data, ix_m2, iy,     buffer.rowStride) << 1;
                    const uint16_t p4 = RAW12(data, ix,    iy,     buffer.rowStride) << 2;
                    const uint16_t p5 = RAW12(data, ix_p2, iy,     buffer.rowStride) << 1;
                    
                    const uint16_t p6 = RAW12(data, ix_m2, iy_p2,  buffer.rowStride);
                    const uint16_t p7 = RAW12(data, ix,    iy_p2,  buffer.rowStride) << 1;
                    const uint16_t p8 = RAW12(data, ix_p2, iy_p2,  buffer.rowStride);

                    const uint16_t out = (p0 + p1 + p2 + p3 + p4 + p5 + p6 + p7 + p8) >> 4;
                    
                    const uint16_t X = ((x - xstart) >> 2);
                    row0[X] = out;
                }

                {
                    const uint16_t ix = x + 1;

                    const int16_t ix_m2 = (std::max)(0, ix - 2);
                    const int16_t ix_p2 = (ix + 2) % buffer.width;
                    
                    const int16_t iy_m2 = (std::max)(0, iy - 2);
                    const int16_t iy_p2 = (iy + 2) % buffer.height;

                    const uint16_t p0 = RAW12(data, ix_m2, iy_m2,  buffer.rowStride);
                    const uint16_t p1 = RAW12(data, ix,    iy_m2,  buffer.rowStride) << 1;
                    const uint16_t p2 = RAW12(data, ix_p2, iy_m2,  buffer.rowStride);
                    
                    const uint16_t p3 = RAW12(data, ix_m2, iy,     buffer.rowStride) << 1;
                    const uint16_t p4 = RAW12(data, ix,    iy,     buffer.rowStride) << 2;
                    const uint16_t p5 = RAW12(data, ix_p2, iy,     buffer.rowStride) << 1;
                    
                    const uint16_t p6 = RAW12(data, ix_m2, iy_p2,  buffer.rowStride);
                    const uint16_t p7 = RAW12(data, ix,    iy_p2,  buffer.rowStride) << 1;
                    const uint16_t p8 = RAW12(data, ix_p2, iy_p2,  buffer.rowStride);

                    const uint16_t out = (p0 + p1 + p2 + p3 + p4 + p5 + p6 + p7 + p8) >> 4;

                    const uint16_t X = ((x - xstart) >> 2) + (binnedWidth >> 1);
                    row0[X] = out;
                }

                // Next row
                iy = y + 1;

                {
                    const uint16_t ix = x;
                    
                    const int16_t ix_m2 = (std::max)(0, ix - 2);
                    const int16_t ix_p2 = (ix + 2) % buffer.width;
                    
                    const int16_t iy_m2 = (std::max)(0, iy - 2);
                    const int16_t iy_p2 = (iy + 2) % buffer.height;

                    const uint16_t p0 = RAW12(data, ix_m2, iy_m2,  buffer.rowStride);
                    const uint16_t p1 = RAW12(data, ix,    iy_m2,  buffer.rowStride) << 1;
                    const uint16_t p2 = RAW12(data, ix_p2, iy_m2,  buffer.rowStride);
                    
                    const uint16_t p3 = RAW12(data, ix_m2, iy,     buffer.rowStride) << 1;
                    const uint16_t p4 = RAW12(data, ix,    iy,     buffer.rowStride) << 2;
                    const uint16_t p5 = RAW12(data, ix_p2, iy,     buffer.rowStride) << 1;
                    
                    const uint16_t p6 = RAW12(data, ix_m2, iy_p2,  buffer.rowStride);
                    const uint16_t p7 = RAW12(data, ix,    iy_p2,  buffer.rowStride) << 1;
                    const uint16_t p8 = RAW12(data, ix_p2, iy_p2,  buffer.rowStride);

                    const uint16_t out = (p0 + p1 + p2 + p3 + p4 + p5 + p6 + p7 + p8) >> 4;

                    const uint16_t X = ((x - xstart) >> 2);
                    row1[X] = out;
                }
                
                {
                    const uint16_t ix = x + 1;
                    
                    const int16_t ix_m2 = (std::max)(0, ix - 2);
                    const int16_t ix_p2 = (ix + 2) % buffer.width;
                    
                    const int16_t iy_m2 = (std::max)(0, iy - 2);
                    const int16_t iy_p2 = (iy + 2) % buffer.height;

                    const uint16_t p0 = RAW12(data, ix_m2, iy_m2,  buffer.rowStride);
                    const uint16_t p1 = RAW12(data, ix,    iy_m2,  buffer.rowStride) << 1;
                    const uint16_t p2 = RAW12(data, ix_p2, iy_m2,  buffer.rowStride);
                    
                    const uint16_t p3 = RAW12(data, ix_m2, iy,     buffer.rowStride) << 1;
                    const uint16_t p4 = RAW12(data, ix,    iy,     buffer.rowStride) << 2;
                    const uint16_t p5 = RAW12(data, ix_p2, iy,     buffer.rowStride) << 1;
                    
                    const uint16_t p6 = RAW12(data, ix_m2, iy_p2,  buffer.rowStride);
                    const uint16_t p7 = RAW12(data, ix,    iy_p2,  buffer.rowStride) << 1;
                    const uint16_t p8 = RAW12(data, ix_p2, iy_p2,  buffer.rowStride);

                    const uint16_t out = (p0 + p1 + p2 + p3 + p4 + p5 + p6 + p7 + p8) >> 4;

                    const uint16_t X = ((x - xstart) >> 2) + (binnedWidth >> 1);
                    row1[X] = out;
                }
            }

            size_t writtenBytes = encodeFunc(row0.data(), row0.size(), data+offset);
            offset += writtenBytes;
            
            writtenBytes = encodeFunc(row1.data(), row1.size(), data+offset);
            offset += writtenBytes;
        }
        
        return offset;
    }

    size_t RawBufferStreamer::cropAndBin_RAW16(RawImageBuffer& buffer,
                                               uint8_t* data,
                                               const int16_t ystart,
                                               const int16_t yend,
                                               const int16_t xstart,
                                               const int16_t xend,
                                               const int16_t binnedWidth) const
    {
        std::vector<uint16_t> row0(binnedWidth);
        std::vector<uint16_t> row1(binnedWidth);
        uint32_t offset = 0;
        auto* encodeFunc = &bitnzpack128v16;

        for(int16_t y = ystart; y < yend; y+=4) {
            for(int16_t x = xstart; x < xend; x+=4) {
                uint16_t iy = y;

                // Unroll inner loops
                {
                    const int16_t ix = x;
                    
                    const int16_t ix_m2 = (std::max)(0, ix - 2);
                    const int16_t ix_p2 = (ix + 2) % buffer.width;
                    
                    const int16_t iy_m2 = (std::max)(0, iy - 2);
                    const int16_t iy_p2 = (iy + 2) % buffer.height;
                    
                    const uint16_t p0 = RAW16(data, ix_m2, iy_m2,  buffer.rowStride);
                    const uint16_t p1 = RAW16(data, ix,    iy_m2,  buffer.rowStride) << 1;
                    const uint16_t p2 = RAW16(data, ix_p2, iy_m2,  buffer.rowStride);
                    
                    const uint16_t p3 = RAW16(data, ix_m2, iy,     buffer.rowStride) << 1;
                    const uint16_t p4 = RAW16(data, ix,    iy,     buffer.rowStride) << 2;
                    const uint16_t p5 = RAW16(data, ix_p2, iy,     buffer.rowStride) << 1;
                    
                    const uint16_t p6 = RAW16(data, ix_m2, iy_p2,  buffer.rowStride);
                    const uint16_t p7 = RAW16(data, ix,    iy_p2,  buffer.rowStride) << 1;
                    const uint16_t p8 = RAW16(data, ix_p2, iy_p2,  buffer.rowStride);

                    const uint16_t out = (p0 + p1 + p2 + p3 + p4 + p5 + p6 + p7 + p8) >> 4;
                    
                    const uint16_t X = ((x - xstart) >> 2);
                    row0[X] = out;
                }

                {
                    const uint16_t ix = x + 1;

                    const int16_t ix_m2 = (std::max)(0, ix - 2);
                    const int16_t ix_p2 = (ix + 2) % buffer.width;
                    
                    const int16_t iy_m2 = (std::max)(0, iy - 2);
                    const int16_t iy_p2 = (iy + 2) % buffer.height;

                    const uint16_t p0 = RAW16(data, ix_m2, iy_m2,  buffer.rowStride);
                    const uint16_t p1 = RAW16(data, ix,    iy_m2,  buffer.rowStride) << 1;
                    const uint16_t p2 = RAW16(data, ix_p2, iy_m2,  buffer.rowStride);
                    
                    const uint16_t p3 = RAW16(data, ix_m2, iy,     buffer.rowStride) << 1;
                    const uint16_t p4 = RAW16(data, ix,    iy,     buffer.rowStride) << 2;
                    const uint16_t p5 = RAW16(data, ix_p2, iy,     buffer.rowStride) << 1;
                    
                    const uint16_t p6 = RAW16(data, ix_m2, iy_p2,  buffer.rowStride);
                    const uint16_t p7 = RAW16(data, ix,    iy_p2,  buffer.rowStride) << 1;
                    const uint16_t p8 = RAW16(data, ix_p2, iy_p2,  buffer.rowStride);

                    const uint16_t out = (p0 + p1 + p2 + p3 + p4 + p5 + p6 + p7 + p8) >> 4;

                    const uint16_t X = ((x - xstart) >> 2) + (binnedWidth >> 1);
                    row0[X] = out;
                }

                // Next row
                iy = y + 1;

                {
                    const uint16_t ix = x;
                    
                    const int16_t ix_m2 = (std::max)(0, ix - 2);
                    const int16_t ix_p2 = (ix + 2) % buffer.width;
                    
                    const int16_t iy_m2 = (std::max)(0, iy - 2);
                    const int16_t iy_p2 = (iy + 2) % buffer.height;

                    const uint16_t p0 = RAW16(data, ix_m2, iy_m2,  buffer.rowStride);
                    const uint16_t p1 = RAW16(data, ix,    iy_m2,  buffer.rowStride) << 1;
                    const uint16_t p2 = RAW16(data, ix_p2, iy_m2,  buffer.rowStride);
                    
                    const uint16_t p3 = RAW16(data, ix_m2, iy,     buffer.rowStride) << 1;
                    const uint16_t p4 = RAW16(data, ix,    iy,     buffer.rowStride) << 2;
                    const uint16_t p5 = RAW16(data, ix_p2, iy,     buffer.rowStride) << 1;
                    
                    const uint16_t p6 = RAW16(data, ix_m2, iy_p2,  buffer.rowStride);
                    const uint16_t p7 = RAW16(data, ix,    iy_p2,  buffer.rowStride) << 1;
                    const uint16_t p8 = RAW16(data, ix_p2, iy_p2,  buffer.rowStride);

                    const uint16_t out = (p0 + p1 + p2 + p3 + p4 + p5 + p6 + p7 + p8) >> 4;

                    const uint16_t X = ((x - xstart) >> 2);
                    row1[X] = out;
                }
                
                {
                    const uint16_t ix = x + 1;
                    
                    const int16_t ix_m2 = (std::max)(0, ix - 2);
                    const int16_t ix_p2 = (ix + 2) % buffer.width;
                    
                    const int16_t iy_m2 = (std::max)(0, iy - 2);
                    const int16_t iy_p2 = (iy + 2) % buffer.height;

                    const uint16_t p0 = RAW16(data, ix_m2, iy_m2,  buffer.rowStride);
                    const uint16_t p1 = RAW16(data, ix,    iy_m2,  buffer.rowStride) << 1;
                    const uint16_t p2 = RAW16(data, ix_p2, iy_m2,  buffer.rowStride);
                    
                    const uint16_t p3 = RAW16(data, ix_m2, iy,     buffer.rowStride) << 1;
                    const uint16_t p4 = RAW16(data, ix,    iy,     buffer.rowStride) << 2;
                    const uint16_t p5 = RAW16(data, ix_p2, iy,     buffer.rowStride) << 1;
                    
                    const uint16_t p6 = RAW16(data, ix_m2, iy_p2,  buffer.rowStride);
                    const uint16_t p7 = RAW16(data, ix,    iy_p2,  buffer.rowStride) << 1;
                    const uint16_t p8 = RAW16(data, ix_p2, iy_p2,  buffer.rowStride);

                    const uint16_t out = (p0 + p1 + p2 + p3 + p4 + p5 + p6 + p7 + p8) >> 4;

                    const uint16_t X = ((x - xstart) >> 2) + (binnedWidth >> 1);
                    row1[X] = out;
                }
            }

            size_t writtenBytes = encodeFunc(row0.data(), row0.size(), data+offset);
            offset += writtenBytes;
            
            writtenBytes = encodeFunc(row1.data(), row1.size(), data+offset);
            offset += writtenBytes;
        }
        
        return offset;
    }

    void RawBufferStreamer::cropAndBin(RawImageBuffer& buffer) const {
//        Measure m("cropAndBin");
        
        const int horizontalCrop = static_cast<const int>(4 * (lround(0.5f * (mCropWidth/100.0f * buffer.width)) / 4));

        // Even vertical crop to match bayer pattern
        const int verticalCrop   = static_cast<const int>(2 * (lround(0.5f * (mCropHeight/100.0f * buffer.height)) / 2));

        uint32_t croppedWidth  = static_cast<const int>(buffer.width - horizontalCrop*2);
        uint32_t croppedHeight = static_cast<const int>(buffer.height - verticalCrop*2);
        
        const int ystart = verticalCrop;
        const int yend = buffer.height - ystart;

        const uint32_t xstart = horizontalCrop;
        const uint32_t xend = buffer.width - horizontalCrop;

        std::vector<uint16_t> row0(croppedWidth / 2);
        std::vector<uint16_t> row1(croppedWidth / 2);

        auto data = buffer.data->lock(true);
        size_t end = 0;

        if(buffer.pixelFormat == PixelFormat::RAW10) {
            end = cropAndBin_RAW10(buffer, data, ystart, yend, xstart, xend, croppedWidth / 2);
        }
        else if(buffer.pixelFormat == PixelFormat::RAW12) {
            end = cropAndBin_RAW12(buffer, data, ystart, yend, xstart, xend, croppedWidth / 2);
        }
        else if(buffer.pixelFormat == PixelFormat::RAW16) {
            end = cropAndBin_RAW16(buffer, data, ystart, yend, xstart, xend, croppedWidth / 2);
        }
        else {
            // Not supported
            buffer.data->unlock();
            return;
        }

        buffer.data->unlock();

        buffer.width = croppedWidth / 2;
        buffer.height = croppedHeight / 2;
        buffer.rowStride = 2 * buffer.width;
        buffer.pixelFormat = PixelFormat::RAW16;
        buffer.isCompressed = true;
        buffer.compressionType = CompressionType::BITNZPACK;
        
        // Update valid range
        buffer.data->setValidRange(0, end);
    }

    void RawBufferStreamer::crop(RawImageBuffer& buffer) const {
        Measure m("crop");

        const int horizontalCrop = static_cast<const int>(4 * (lround(0.5f * (mCropWidth/100.0f * buffer.width)) / 4));

        // Even vertical crop to match bayer pattern
        const int verticalCrop   = static_cast<const int>(2 * (lround(0.5f * (mCropHeight/100.0f * buffer.height)) / 2));

        uint32_t croppedWidth  = static_cast<const int>(buffer.width - horizontalCrop*2);
        uint32_t croppedHeight = static_cast<const int>(buffer.height - verticalCrop*2);
        
        auto data = buffer.data->lock(true);

        const int xstart = horizontalCrop;
        const int xend = buffer.width - xstart;

        const int ystart = verticalCrop;
        const int yend = buffer.height - ystart;

        const uint16_t croppedWidth_2 = croppedWidth>>1;

        std::vector<uint16_t> row(croppedWidth);
        size_t offset = 0;
        
        auto* encodeFunc = &bitnzpack128v16;
        auto compressionType = CompressionType::BITNZPACK;

        // TODO: Unsafely assuming here that the compressed size is always smaller than the input.

        if(buffer.pixelFormat == PixelFormat::RAW10) {
            for(int y = ystart; y < yend; y++) {
                for(int x = xstart; x < xend; x+=2) {
                    const uint16_t p0 = RAW10(data, x,   y, buffer.rowStride);
                    const uint16_t p1 = RAW10(data, x+1, y, buffer.rowStride);

                    int X = (x - xstart) >> 1;

                    row[X]                  = p0;
                    row[croppedWidth_2 + X] = p1;
                }
                
                size_t writtenBytes = encodeFunc(row.data(), row.size(), data+offset);
                offset += writtenBytes;
            }
        }
        else if(buffer.pixelFormat == PixelFormat::RAW12) {
            for(int y = ystart; y < yend; y++) {
                for(int x = xstart; x < xend; x+=2) {
                    const uint16_t p0 = RAW12(data, x,   y, buffer.rowStride);
                    const uint16_t p1 = RAW12(data, x+1, y, buffer.rowStride);
                    
                    int X = (x - xstart) >> 1;
                    
                    row[X]                  = p0;
                    row[croppedWidth_2 + X] = p1;
                }

                size_t writtenBytes = encodeFunc(row.data(), row.size(), data+offset);
                offset += writtenBytes;
            }
        }
        else if(buffer.pixelFormat == PixelFormat::RAW16) {
            for(int y = ystart; y < yend; y++) {
                for(int x = xstart; x < xend; x+=2) {
                    const uint16_t p0 = RAW16(data, x,   y, buffer.rowStride);
                    const uint16_t p1 = RAW16(data, x+1, y, buffer.rowStride);
                    
                    int X = (x - xstart) >> 1;
                    
                    row[X]                  = p0;
                    row[croppedWidth_2 + X] = p1;
                }

                size_t writtenBytes = encodeFunc(row.data(), row.size(), data+offset);
                offset += writtenBytes;
            }
        }
        else {
            // Not supported
            buffer.data->unlock();
            return;
        }

        buffer.data->unlock();

        // Update buffer
        buffer.pixelFormat = PixelFormat::RAW16;
        buffer.rowStride = croppedWidth*2;
        buffer.width = croppedWidth;
        buffer.height = croppedHeight;
        buffer.isCompressed = true;
        buffer.compressionType = compressionType;

        buffer.data->setValidRange(0, offset);
    }

    void RawBufferStreamer::processBuffer(std::shared_ptr<RawImageBuffer> buffer) {
        if(mBin)
            cropAndBin(*buffer);
        else
            crop(*buffer);
    }

    void RawBufferStreamer::doProcess() {
        std::shared_ptr<RawImageBuffer> buffer;
        
        while(mRunning) {
            if(!mUnprocessedBuffers.wait_dequeue_timed(buffer, std::chrono::milliseconds(67))) {
                continue;
            }
            
            processBuffer(buffer);
            
            // Add to the ready list
            mReadyBuffers.enqueue(buffer);
        }

    }

    void RawBufferStreamer::doStream(const int fd, const RawCameraMetadata& cameraMetadata, const int numContainers) {
        util::CloseableFd fdContext(fd);
        
        std::shared_ptr<RawImageBuffer> buffer;
        RawContainer container(cameraMetadata, numContainers);
        size_t start, end;

        if(!container.create(fd)) {
            logger::log("Failed to create container");
            // TODO: report back error
            return;
        }

        while(mRunning) {
            if(!mReadyBuffers.wait_dequeue_timed(buffer, std::chrono::milliseconds(100))) {
                continue;
            }

            if(!container.append(fd, *buffer)) {
                logger::log("Failed to append buffer");
                RawBufferManager::get().discardBuffer(buffer);
                return;
            }

            start = end = 0;
            buffer->data->getValidRange(start, end);

            // Return the buffer after it has been written
            RawBufferManager::get().discardBuffer(buffer);

            mWrittenBytes += (end - start);
            mWrittenFrames++;
        }

        //
        // Flush buffers
        //

        // Ready buffers
        while(mReadyBuffers.try_dequeue(buffer)) {
            if(!container.append(fd, *buffer)) {
                logger::log("Failed to flush ready buffer");
                RawBufferManager::get().discardBuffer(buffer);
                return;
            }

            buffer->data->getValidRange(start, end);
            mWrittenBytes += (end - start);
            mWrittenFrames++;

            RawBufferManager::get().discardBuffer(buffer);
        }

        // Unprocessed buffers
        while(mUnprocessedBuffers.try_dequeue(buffer)) {
            processBuffer(buffer);
            
            if(!container.append(fd, *buffer)) {
                logger::log("Failed to flush unprocessed buffer");
                RawBufferManager::get().discardBuffer(buffer);
                return;
            }

            buffer->data->getValidRange(start, end);
            mWrittenBytes += (end - start);
            mWrittenFrames++;

            RawBufferManager::get().discardBuffer(buffer);
        }

        container.commit(fd);
    }

    bool RawBufferStreamer::isRunning() const {
        return mRunning;
    }

    float RawBufferStreamer::estimateFps() const {
        auto now = std::chrono::steady_clock::now();
        float durationSecs = std::chrono::duration <float>(now - mStartTime).count();
        
        return mAcceptedFrames / (1e-5f + durationSecs);
    }

    size_t RawBufferStreamer::writenOutputBytes() const {
        return mWrittenBytes;
    }
}
