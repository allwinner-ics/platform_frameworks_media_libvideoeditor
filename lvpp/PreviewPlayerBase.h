/*
 * Copyright (C) 2011 The Android Open Source Project
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

#ifndef PREVIEW_PLAYER_BASE_H_

#define PREVIEW_PLAYER_BASE_H_

#include "HTTPBase.h"
#include "TimedEventQueue.h"

#include <media/MediaPlayerInterface.h>
#include <media/stagefright/DataSource.h>
#include <media/stagefright/MediaSource.h>
#include <media/stagefright/OMXClient.h>
#include <media/stagefright/TimeSource.h>
#include <utils/threads.h>
#include <drm/DrmManagerClient.h>

namespace android {

struct AudioPlayerBase;
struct DataSource;
struct MediaBuffer;
struct MediaExtractor;
struct MediaSource;
struct NuCachedSource2;
struct ISurfaceTexture;

class DrmManagerClinet;
class DecryptHandle;

struct AwesomeRenderer : public RefBase {
    AwesomeRenderer() {}

    virtual void render(MediaBuffer *buffer) = 0;

private:
    AwesomeRenderer(const AwesomeRenderer &);
    AwesomeRenderer &operator=(const AwesomeRenderer &);
};

struct PreviewPlayerBase {
    PreviewPlayerBase();
    ~PreviewPlayerBase();

    void setListener(const wp<MediaPlayerBase> &listener);

    status_t setDataSource(
            const char *uri,
            const KeyedVector<String8, String8> *headers = NULL);

    status_t setDataSource(int fd, int64_t offset, int64_t length);

    status_t setDataSource(const sp<IStreamSource> &source);

    void reset();

    status_t prepare();
    status_t prepare_l();
    status_t prepareAsync();
    status_t prepareAsync_l();

    status_t play();
    status_t pause();

    bool isPlaying() const;

    void setSurface(const sp<Surface> &surface);
    void setSurfaceTexture(const sp<ISurfaceTexture> &surfaceTexture);
    void setAudioSink(const sp<MediaPlayerBase::AudioSink> &audioSink);
    status_t setLooping(bool shouldLoop);

    status_t getDuration(int64_t *durationUs);
    status_t getPosition(int64_t *positionUs);

    status_t setParameter(int key, const Parcel &request);
    status_t getParameter(int key, Parcel *reply);

    status_t seekTo(int64_t timeUs);

    // This is a mask of MediaExtractor::Flags.
    uint32_t flags() const;

    void postAudioEOS(int64_t delayUs = 0ll);
    void postAudioSeekComplete();

private:
    friend struct AwesomeEvent;
    friend struct PreviewPlayer;

    enum {
        PLAYING             = 1,
        LOOPING             = 2,
        FIRST_FRAME         = 4,
        PREPARING           = 8,
        PREPARED            = 16,
        AT_EOS              = 32,
        PREPARE_CANCELLED   = 64,
        CACHE_UNDERRUN      = 128,
        AUDIO_AT_EOS        = 256,
        VIDEO_AT_EOS        = 512,
        AUTO_LOOPING        = 1024,

        // We are basically done preparing but are currently buffering
        // sufficient data to begin playback and finish the preparation phase
        // for good.
        PREPARING_CONNECTED = 2048,

        // We're triggering a single video event to display the first frame
        // after the seekpoint.
        SEEK_PREVIEW        = 4096,

        AUDIO_RUNNING       = 8192,
        AUDIOPLAYER_STARTED = 16384,

        INCOGNITO           = 32768,
    };

    mutable Mutex mLock;
    Mutex mMiscStateLock;

    OMXClient mClient;
    TimedEventQueue mQueue;
    bool mQueueStarted;
    wp<MediaPlayerBase> mListener;

    sp<Surface> mSurface;
    sp<ANativeWindow> mNativeWindow;
    sp<MediaPlayerBase::AudioSink> mAudioSink;

    SystemTimeSource mSystemTimeSource;
    TimeSource *mTimeSource;

    String8 mUri;
    KeyedVector<String8, String8> mUriHeaders;

    sp<DataSource> mFileSource;

    sp<MediaSource> mVideoTrack;
    sp<MediaSource> mVideoSource;
    sp<AwesomeRenderer> mVideoRenderer;
    bool mVideoRendererIsPreview;

    sp<MediaSource> mAudioTrack;
    sp<MediaSource> mAudioSource;
    AudioPlayerBase *mAudioPlayer;
    int64_t mDurationUs;

    int32_t mDisplayWidth;
    int32_t mDisplayHeight;

    uint32_t mFlags;
    uint32_t mExtractorFlags;

    int64_t mTimeSourceDeltaUs;
    int64_t mVideoTimeUs;

    enum SeekType {
        NO_SEEK,
        SEEK,
        SEEK_VIDEO_ONLY
    };
    SeekType mSeeking;

    bool mSeekNotificationSent;
    int64_t mSeekTimeUs;

    int64_t mBitrate;  // total bitrate of the file (in bps) or -1 if unknown.

    bool mWatchForAudioSeekComplete;
    bool mWatchForAudioEOS;

    sp<TimedEventQueue::Event> mVideoEvent;
    bool mVideoEventPending;
    sp<TimedEventQueue::Event> mStreamDoneEvent;
    bool mStreamDoneEventPending;
    sp<TimedEventQueue::Event> mBufferingEvent;
    bool mBufferingEventPending;
    sp<TimedEventQueue::Event> mCheckAudioStatusEvent;
    bool mAudioStatusEventPending;
    sp<TimedEventQueue::Event> mVideoLagEvent;
    bool mVideoLagEventPending;

    sp<TimedEventQueue::Event> mAsyncPrepareEvent;
    Condition mPreparedCondition;
    bool mIsAsyncPrepare;
    status_t mPrepareResult;
    status_t mStreamDoneStatus;

    void postVideoEvent_l(int64_t delayUs = -1);
    void postBufferingEvent_l();
    void postStreamDoneEvent_l(status_t status);
    void postCheckAudioStatusEvent_l(int64_t delayUs);
    void postVideoLagEvent_l();
    status_t play_l();

    MediaBuffer *mVideoBuffer;

    sp<HTTPBase> mConnectingDataSource;
    sp<NuCachedSource2> mCachedSource;

    DrmManagerClient *mDrmManagerClient;
    sp<DecryptHandle> mDecryptHandle;

    int64_t mLastVideoTimeUs;

    ARect mCropRect;
    int32_t mGivenWidth, mGivenHeight;

    status_t setDataSource_l(
            const char *uri,
            const KeyedVector<String8, String8> *headers = NULL);

    status_t setDataSource_l(const sp<DataSource> &dataSource);
    status_t setDataSource_l(const sp<MediaExtractor> &extractor);
    void reset_l();
    status_t seekTo_l(int64_t timeUs);
    status_t pause_l(bool at_eos = false);
    void initRenderer_l();
    void notifyVideoSize_l();
    void seekAudioIfNecessary_l();

    void cancelPlayerEvents(bool keepBufferingGoing = false);

    void setAudioSource(sp<MediaSource> source);
    status_t initAudioDecoder();

    void setVideoSource(sp<MediaSource> source);
    status_t initVideoDecoder(uint32_t flags = 0);

    void onStreamDone();

    void notifyListener_l(int msg, int ext1 = 0, int ext2 = 0);

    void onVideoEvent();
    void onBufferingUpdate();
    void onCheckAudioStatus();
    void onPrepareAsyncEvent();
    void abortPrepare(status_t err);
    void finishAsyncPrepare_l();
    void onVideoLagUpdate();

    bool getCachedDuration_l(int64_t *durationUs, bool *eos);

    status_t finishSetDataSource_l();

    static bool ContinuePreparation(void *cookie);

    bool getBitrate(int64_t *bitrate);

    void finishSeekIfNecessary(int64_t videoTimeUs);
    void ensureCacheIsFetching_l();

    status_t startAudioPlayer_l();

    void shutdownVideoDecoder_l();
    void setNativeWindow_l(const sp<ANativeWindow> &native);

    PreviewPlayerBase(const PreviewPlayerBase &);
    PreviewPlayerBase &operator=(const PreviewPlayerBase &);
};

}  // namespace android

#endif  // PREVIEW_PLAYER_BASE_H_

