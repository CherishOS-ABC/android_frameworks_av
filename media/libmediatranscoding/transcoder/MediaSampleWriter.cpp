/*
 * Copyright (C) 2020 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// #define LOG_NDEBUG 0
#define LOG_TAG "MediaSampleWriter"

#include <android-base/logging.h>
#include <media/MediaSampleWriter.h>
#include <media/NdkMediaMuxer.h>

namespace android {

class DefaultMuxer : public MediaSampleWriterMuxerInterface {
public:
    // MediaSampleWriterMuxerInterface
    ssize_t addTrack(AMediaFormat* trackFormat) override {
        // If the track format has rotation, need to call AMediaMuxer_setOrientationHint
        // to set the rotation. Muxer doesn't take rotation specified on the track.
        const char* mime;
        if (AMediaFormat_getString(trackFormat, AMEDIAFORMAT_KEY_MIME, &mime) &&
            strncmp(mime, "video/", 6) == 0) {
            int32_t rotation;
            if (AMediaFormat_getInt32(trackFormat, AMEDIAFORMAT_KEY_ROTATION, &rotation) &&
                (rotation != 0)) {
                AMediaMuxer_setOrientationHint(mMuxer, rotation);
            }
        }

        return AMediaMuxer_addTrack(mMuxer, trackFormat);
    }
    media_status_t start() override { return AMediaMuxer_start(mMuxer); }
    media_status_t writeSampleData(size_t trackIndex, const uint8_t* data,
                                   const AMediaCodecBufferInfo* info) override {
        return AMediaMuxer_writeSampleData(mMuxer, trackIndex, data, info);
    }
    media_status_t stop() override { return AMediaMuxer_stop(mMuxer); }
    // ~MediaSampleWriterMuxerInterface

    static std::shared_ptr<DefaultMuxer> create(int fd) {
        AMediaMuxer* ndkMuxer = AMediaMuxer_new(fd, AMEDIAMUXER_OUTPUT_FORMAT_MPEG_4);
        if (ndkMuxer == nullptr) {
            LOG(ERROR) << "Unable to create AMediaMuxer";
            return nullptr;
        }

        return std::make_shared<DefaultMuxer>(ndkMuxer);
    }

    ~DefaultMuxer() {
        if (mMuxer != nullptr) {
            AMediaMuxer_delete(mMuxer);
        }
    }

    DefaultMuxer(AMediaMuxer* muxer) : mMuxer(muxer){};
    DefaultMuxer() = delete;

private:
    AMediaMuxer* mMuxer;
};

MediaSampleWriter::~MediaSampleWriter() {
    if (mState == STARTED) {
        stop();  // Join thread.
    }
}

bool MediaSampleWriter::init(int fd, const OnWritingFinishedCallback& callback) {
    return init(DefaultMuxer::create(fd), callback);
}

bool MediaSampleWriter::init(const std::shared_ptr<MediaSampleWriterMuxerInterface>& muxer,
                             const OnWritingFinishedCallback& callback) {
    if (callback == nullptr) {
        LOG(ERROR) << "Callback cannot be null";
        return false;
    } else if (muxer == nullptr) {
        LOG(ERROR) << "Muxer cannot be null";
        return false;
    }

    std::scoped_lock lock(mStateMutex);
    if (mState != UNINITIALIZED) {
        LOG(ERROR) << "Sample writer is already initialized";
        return false;
    }

    mState = INITIALIZED;
    mMuxer = muxer;
    mWritingFinishedCallback = callback;
    return true;
}

bool MediaSampleWriter::addTrack(const std::shared_ptr<MediaSampleQueue>& sampleQueue,
                                 const std::shared_ptr<AMediaFormat>& trackFormat) {
    if (sampleQueue == nullptr || trackFormat == nullptr) {
        LOG(ERROR) << "Sample queue and track format must be non-null";
        return false;
    }

    std::scoped_lock lock(mStateMutex);
    if (mState != INITIALIZED) {
        LOG(ERROR) << "Muxer needs to be initialized when adding tracks.";
        return false;
    }
    ssize_t trackIndex = mMuxer->addTrack(trackFormat.get());
    if (trackIndex < 0) {
        LOG(ERROR) << "Failed to add media track to muxer: " << trackIndex;
        return false;
    }

    int64_t durationUs;
    if (!AMediaFormat_getInt64(trackFormat.get(), AMEDIAFORMAT_KEY_DURATION, &durationUs)) {
        durationUs = 0;
    }

    mTracks.emplace_back(sampleQueue, static_cast<size_t>(trackIndex), durationUs);
    return true;
}

bool MediaSampleWriter::start() {
    std::scoped_lock lock(mStateMutex);

    if (mTracks.size() == 0) {
        LOG(ERROR) << "No tracks to write.";
        return false;
    } else if (mState != INITIALIZED) {
        LOG(ERROR) << "Sample writer is not initialized";
        return false;
    }

    mThread = std::thread([this] {
        media_status_t status = writeSamples();
        mWritingFinishedCallback(status);
    });
    mState = STARTED;
    return true;
}

bool MediaSampleWriter::stop() {
    std::scoped_lock lock(mStateMutex);

    if (mState != STARTED) {
        LOG(ERROR) << "Sample writer is not started.";
        return false;
    }

    // Stop the sources, and wait for thread to join.
    for (auto& track : mTracks) {
        track.mSampleQueue->abort();
    }
    mThread.join();
    mState = STOPPED;
    return true;
}

media_status_t MediaSampleWriter::writeSamples() {
    media_status_t muxerStatus = mMuxer->start();
    if (muxerStatus != AMEDIA_OK) {
        LOG(ERROR) << "Error starting muxer: " << muxerStatus;
        return muxerStatus;
    }

    media_status_t writeStatus = runWriterLoop();
    if (writeStatus != AMEDIA_OK) {
        LOG(ERROR) << "Error writing samples: " << writeStatus;
    }

    muxerStatus = mMuxer->stop();
    if (muxerStatus != AMEDIA_OK) {
        LOG(ERROR) << "Error stopping muxer: " << muxerStatus;
    }

    return writeStatus != AMEDIA_OK ? writeStatus : muxerStatus;
}

media_status_t MediaSampleWriter::runWriterLoop() {
    AMediaCodecBufferInfo bufferInfo;
    uint32_t segmentEndTimeUs = mTrackSegmentLengthUs;
    bool samplesLeft = true;

    while (samplesLeft) {
        samplesLeft = false;
        for (auto& track : mTracks) {
            if (track.mReachedEos) continue;

            std::shared_ptr<MediaSample> sample;
            do {
                if (track.mSampleQueue->dequeue(&sample)) {
                    // Track queue was aborted.
                    return AMEDIA_ERROR_UNKNOWN;  // TODO(lnilsson): Custom error code.
                } else if (sample->info.flags & SAMPLE_FLAG_END_OF_STREAM) {
                    // Track reached end of stream.
                    track.mReachedEos = true;

                    // Preserve source track duration by setting the appropriate timestamp on the
                    // empty End-Of-Stream sample.
                    if (track.mDurationUs > 0 && track.mFirstSampleTimeSet) {
                        sample->info.presentationTimeUs =
                                track.mDurationUs + track.mFirstSampleTimeUs;
                    }
                } else {
                    samplesLeft = true;
                }

                // Record the first sample's timestamp in order to translate duration to EOS time
                // for tracks that does not start at 0.
                if (!track.mFirstSampleTimeSet) {
                    track.mFirstSampleTimeUs = sample->info.presentationTimeUs;
                    track.mFirstSampleTimeSet = true;
                }

                bufferInfo.offset = sample->dataOffset;
                bufferInfo.size = sample->info.size;
                bufferInfo.flags = sample->info.flags;
                bufferInfo.presentationTimeUs = sample->info.presentationTimeUs;

                media_status_t status =
                        mMuxer->writeSampleData(track.mTrackIndex, sample->buffer, &bufferInfo);
                if (status != AMEDIA_OK) {
                    LOG(ERROR) << "writeSampleData returned " << status;
                    return status;
                }

            } while (sample->info.presentationTimeUs < segmentEndTimeUs && !track.mReachedEos);
        }

        segmentEndTimeUs += mTrackSegmentLengthUs;
    }

    return AMEDIA_OK;
}
}  // namespace android
