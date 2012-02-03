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


#define LOG_NDEBUG 1
#define LOG_TAG "PreviewPlayer"
#include <utils/Log.h>

#include <dlfcn.h>

#include "PreviewPlayer.h"
#include "DummyAudioSource.h"
#include "DummyVideoSource.h"
#include "VideoEditorSRC.h"
#include "include/NuCachedSource2.h"
#include "include/ThrottledSource.h"


#include <binder/IPCThreadState.h>
#include <media/stagefright/DataSource.h>
#include <media/stagefright/FileSource.h>
#include <media/stagefright/MediaBuffer.h>
#include <media/stagefright/MediaDefs.h>
#include <media/stagefright/MediaExtractor.h>
#include <media/stagefright/MediaDebug.h>
#include <media/stagefright/MediaSource.h>
#include <media/stagefright/MetaData.h>
#include <media/stagefright/OMXCodec.h>

#include <surfaceflinger/Surface.h>
#include <media/stagefright/foundation/ALooper.h>

namespace android {


struct PreviewPlayerEvent : public TimedEventQueue::Event {
    PreviewPlayerEvent(
            PreviewPlayer *player,
            void (PreviewPlayer::*method)())
        : mPlayer(player),
          mMethod(method) {
    }

protected:
    virtual ~PreviewPlayerEvent() {}

    virtual void fire(TimedEventQueue *queue, int64_t /* now_us */) {
        (mPlayer->*mMethod)();
    }

private:
    PreviewPlayer *mPlayer;
    void (PreviewPlayer::*mMethod)();

    PreviewPlayerEvent(const PreviewPlayerEvent &);
    PreviewPlayerEvent &operator=(const PreviewPlayerEvent &);
};

PreviewPlayer::PreviewPlayer(NativeWindowRenderer* renderer)
    : PreviewPlayerBase(),
      mNativeWindowRenderer(renderer),
      mCurrFramingEffectIndex(0),
      mFrameRGBBuffer(NULL),
      mFrameYUVBuffer(NULL) {

    mVideoRenderer = NULL;
    mEffectsSettings = NULL;
    mVeAudioPlayer = NULL;
    mAudioMixStoryBoardTS = 0;
    mCurrentMediaBeginCutTime = 0;
    mCurrentMediaVolumeValue = 0;
    mNumberEffects = 0;
    mDecodedVideoTs = 0;
    mDecVideoTsStoryBoard = 0;
    mCurrentVideoEffect = VIDEO_EFFECT_NONE;
    mProgressCbInterval = 0;
    mNumberDecVideoFrames = 0;
    mOverlayUpdateEventPosted = false;
    mIsChangeSourceRequired = true;

    mVideoEvent = new PreviewPlayerEvent(this, &PreviewPlayer::onVideoEvent);
    mVideoEventPending = false;
    mStreamDoneEvent = new PreviewPlayerEvent(this,
         &PreviewPlayer::onStreamDone);

    mStreamDoneEventPending = false;

    mCheckAudioStatusEvent = new PreviewPlayerEvent(
        this, &PreviewPlayerBase::onCheckAudioStatus);

    mAudioStatusEventPending = false;

    mProgressCbEvent = new PreviewPlayerEvent(this,
         &PreviewPlayer::onProgressCbEvent);

    mOverlayUpdateEvent = new PreviewPlayerEvent(this,
        &PreviewPlayer::onUpdateOverlayEvent);
    mProgressCbEventPending = false;

    mOverlayUpdateEventPending = false;
    mRenderingMode = (M4xVSS_MediaRendering)MEDIA_RENDERING_INVALID;
    mIsFiftiesEffectStarted = false;
    reset();
}

PreviewPlayer::~PreviewPlayer() {

    if (mQueueStarted) {
        mQueue.stop();
    }

    reset();

    if (mVideoRenderer) {
        mNativeWindowRenderer->destroyRenderInput(mVideoRenderer);
    }
}

void PreviewPlayer::cancelPlayerEvents(bool keepBufferingGoing) {
    mQueue.cancelEvent(mVideoEvent->eventID());
    mVideoEventPending = false;
    mQueue.cancelEvent(mStreamDoneEvent->eventID());
    mStreamDoneEventPending = false;
    mQueue.cancelEvent(mCheckAudioStatusEvent->eventID());
    mAudioStatusEventPending = false;

    mQueue.cancelEvent(mProgressCbEvent->eventID());
    mProgressCbEventPending = false;
}

status_t PreviewPlayer::setDataSource(
        const char *uri, const KeyedVector<String8, String8> *headers) {
    Mutex::Autolock autoLock(mLock);
    return setDataSource_l(uri, headers);
}

status_t PreviewPlayer::setDataSource_l(
        const char *uri, const KeyedVector<String8, String8> *headers) {
    reset_l();

    mUri = uri;

    if (headers) {
        mUriHeaders = *headers;
    }

    // The actual work will be done during preparation in the call to
    // ::finishSetDataSource_l to avoid blocking the calling thread in
    // setDataSource for any significant time.
    return OK;
}

status_t PreviewPlayer::setDataSource_l(const sp<MediaExtractor> &extractor) {
    bool haveAudio = false;
    bool haveVideo = false;
    for (size_t i = 0; i < extractor->countTracks(); ++i) {
        sp<MetaData> meta = extractor->getTrackMetaData(i);

        const char *mime;
        CHECK(meta->findCString(kKeyMIMEType, &mime));

        if (!haveVideo && !strncasecmp(mime, "video/", 6)) {
            setVideoSource(extractor->getTrack(i));
            haveVideo = true;
        } else if (!haveAudio && !strncasecmp(mime, "audio/", 6)) {
            setAudioSource(extractor->getTrack(i));
            haveAudio = true;

            if (!strcasecmp(mime, MEDIA_MIMETYPE_AUDIO_VORBIS)) {
                // Only do this for vorbis audio, none of the other audio
                // formats even support this ringtone specific hack and
                // retrieving the metadata on some extractors may turn out
                // to be very expensive.
                sp<MetaData> fileMeta = extractor->getMetaData();
                int32_t loop;
                if (fileMeta != NULL
                        && fileMeta->findInt32(kKeyAutoLoop, &loop)
                         && loop != 0) {
                    mFlags |= AUTO_LOOPING;
                }
            }
        }

        if (haveAudio && haveVideo) {
            break;
        }
    }

    /* Add the support for Dummy audio*/
    if( !haveAudio ){
        LOGV("PreviewPlayer: setDataSource_l Dummyaudiocreation started");

        mAudioTrack = DummyAudioSource::Create(32000, 2, 20000,
                                              ((mPlayEndTimeMsec)*1000LL));
        LOGV("PreviewPlayer: setDataSource_l Dummyauiosource created");
        if(mAudioTrack != NULL) {
            haveAudio = true;
        }
    }

    if (!haveAudio && !haveVideo) {
        return UNKNOWN_ERROR;
    }

    mExtractorFlags = extractor->flags();
    return OK;
}

status_t PreviewPlayer::setDataSource_l_jpg() {
    M4OSA_ERR err = M4NO_ERROR;
    LOGV("PreviewPlayer: setDataSource_l_jpg started");

    mAudioSource = DummyAudioSource::Create(32000, 2, 20000,
                                          ((mPlayEndTimeMsec)*1000LL));
    LOGV("PreviewPlayer: setDataSource_l_jpg Dummyaudiosource created");
    if(mAudioSource != NULL) {
        setAudioSource(mAudioSource);
    }
    status_t error = mAudioSource->start();
    if (error != OK) {
        LOGV("Error starting dummy audio source");
        mAudioSource.clear();
        return err;
    }

    mDurationUs = (mPlayEndTimeMsec - mPlayBeginTimeMsec)*1000LL;

    mVideoSource = DummyVideoSource::Create(mVideoWidth, mVideoHeight,
                                            mDurationUs, mUri);

    updateSizeToRender(mVideoSource->getFormat());
    setVideoSource(mVideoSource);
    status_t err1 = mVideoSource->start();
    if (err1 != OK) {
        mVideoSource.clear();
        return err;
    }

    mIsVideoSourceJpg = true;
    return OK;
}

void PreviewPlayer::reset() {
    Mutex::Autolock autoLock(mLock);
    reset_l();
}

void PreviewPlayer::reset_l() {

    if (mFlags & PREPARING) {
        mFlags |= PREPARE_CANCELLED;
    }

    while (mFlags & PREPARING) {
        mPreparedCondition.wait(mLock);
    }

    cancelPlayerEvents();
    mAudioTrack.clear();
    mVideoTrack.clear();

    // Shutdown audio first, so that the respone to the reset request
    // appears to happen instantaneously as far as the user is concerned
    // If we did this later, audio would continue playing while we
    // shutdown the video-related resources and the player appear to
    // not be as responsive to a reset request.
    if (mAudioPlayer == NULL && mAudioSource != NULL) {
        // If we had an audio player, it would have effectively
        // taken possession of the audio source and stopped it when
        // _it_ is stopped. Otherwise this is still our responsibility.
        mAudioSource->stop();
    }
    mAudioSource.clear();

    mTimeSource = NULL;

    //Single audio player instance used
    //So donot delete it here
    //It is deleted from PreviewController class
    //delete mAudioPlayer;
    mAudioPlayer = NULL;

    if (mVideoBuffer) {
        mVideoBuffer->release();
        mVideoBuffer = NULL;
    }

    if (mVideoSource != NULL) {
        mVideoSource->stop();

        // The following hack is necessary to ensure that the OMX
        // component is completely released by the time we may try
        // to instantiate it again.
        wp<MediaSource> tmp = mVideoSource;
        mVideoSource.clear();
        while (tmp.promote() != NULL) {
            usleep(1000);
        }
        IPCThreadState::self()->flushCommands();
    }

    mDurationUs = -1;
    mFlags = 0;
    mExtractorFlags = 0;
    mVideoWidth = mVideoHeight = -1;
    mTimeSourceDeltaUs = 0;
    mVideoTimeUs = 0;

    mSeeking = NO_SEEK;
    mSeekNotificationSent = false;
    mSeekTimeUs = 0;

    mUri.setTo("");
    mUriHeaders.clear();

    mFileSource.clear();

    mCurrentVideoEffect = VIDEO_EFFECT_NONE;
    mIsVideoSourceJpg = false;
    mFrameRGBBuffer = NULL;
    if(mFrameYUVBuffer != NULL) {
        free(mFrameYUVBuffer);
        mFrameYUVBuffer = NULL;
    }
}

status_t PreviewPlayer::play() {
    Mutex::Autolock autoLock(mLock);

    mFlags &= ~CACHE_UNDERRUN;
    mFlags &= ~INFORMED_AV_EOS;
    return play_l();
}

status_t PreviewPlayer::startAudioPlayer_l() {
    CHECK(!(mFlags & AUDIO_RUNNING));

    if (mAudioSource == NULL || mAudioPlayer == NULL) {
        return OK;
    }

    if (!(mFlags & AUDIOPLAYER_STARTED)) {
        mFlags |= AUDIOPLAYER_STARTED;

        // We've already started the MediaSource in order to enable
        // the prefetcher to read its data.
        status_t err = mVeAudioPlayer->start(
                true /* sourceAlreadyStarted */);

        if (err != OK) {
            notifyListener_l(MEDIA_ERROR, MEDIA_ERROR_UNKNOWN, err);
            return err;
        }
    } else {
        mVeAudioPlayer->resume();
    }

    mFlags |= AUDIO_RUNNING;

    mWatchForAudioEOS = true;

    return OK;
}

status_t PreviewPlayer::setAudioPlayer(AudioPlayerBase *audioPlayer) {
    Mutex::Autolock autoLock(mLock);
    CHECK(!(mFlags & PLAYING));
    mAudioPlayer = audioPlayer;

    LOGV("SetAudioPlayer");
    mIsChangeSourceRequired = true;
    mVeAudioPlayer =
            (VideoEditorAudioPlayer*)mAudioPlayer;

    // check if the new and old source are dummy
    sp<MediaSource> anAudioSource = mVeAudioPlayer->getSource();
    if (anAudioSource == NULL) {
        // Audio player does not have any source set.
        LOGV("setAudioPlayer: Audio player does not have any source set");
        return OK;
    }

    // If new video source is not dummy, then always change source
    // Else audio player continues using old audio source and there are
    // frame drops to maintain AV sync
    sp<MetaData> meta;
    if (mVideoSource != NULL) {
        meta = mVideoSource->getFormat();
        const char *pVidSrcType;
        if (meta->findCString(kKeyDecoderComponent, &pVidSrcType)) {
            if (strcmp(pVidSrcType, "DummyVideoSource") != 0) {
                LOGV(" Video clip with silent audio; need to change source");
                return OK;
            }
        }
    }

    const char *pSrcType1;
    const char *pSrcType2;
    meta = anAudioSource->getFormat();

    if (meta->findCString(kKeyDecoderComponent, &pSrcType1)) {
        if (strcmp(pSrcType1, "DummyAudioSource") == 0) {
            meta = mAudioSource->getFormat();
            if (meta->findCString(kKeyDecoderComponent, &pSrcType2)) {
                if (strcmp(pSrcType2, "DummyAudioSource") == 0) {
                    mIsChangeSourceRequired = false;
                    // Just set the new play duration for the existing source
                    MediaSource *pMediaSrc = anAudioSource.get();
                    DummyAudioSource *pDummyAudioSource = (DummyAudioSource*)pMediaSrc;
                    //Increment the duration of audio source
                    pDummyAudioSource->setDuration(
                        (int64_t)((mPlayEndTimeMsec)*1000LL));

                    // Stop the new audio source
                    // since we continue using old source
                    LOGV("setAudioPlayer: stop new audio source");
                    mAudioSource->stop();
                }
            }
        }
    }

    return OK;
}

void PreviewPlayer::onStreamDone() {
    // Posted whenever any stream finishes playing.

    Mutex::Autolock autoLock(mLock);
    if (!mStreamDoneEventPending) {
        return;
    }
    mStreamDoneEventPending = false;

    if (mStreamDoneStatus != ERROR_END_OF_STREAM) {
        LOGV("MEDIA_ERROR %d", mStreamDoneStatus);

        notifyListener_l(
                MEDIA_ERROR, MEDIA_ERROR_UNKNOWN, mStreamDoneStatus);

        pause_l(true /* at eos */);

        mFlags |= AT_EOS;
        return;
    }

    const bool allDone =
        (mVideoSource == NULL || (mFlags & VIDEO_AT_EOS))
            && (mAudioSource == NULL || (mFlags & AUDIO_AT_EOS));

    if (!allDone) {
        return;
    }

    if (mFlags & (LOOPING | AUTO_LOOPING)) {
        seekTo_l(0);

        if (mVideoSource != NULL) {
            postVideoEvent_l();
        }
    } else {
        LOGV("MEDIA_PLAYBACK_COMPLETE");
        //pause before sending event
        pause_l(true /* at eos */);

        //This lock is used to syncronize onStreamDone() in PreviewPlayer and
        //stopPreview() in PreviewController
        Mutex::Autolock autoLock(mLockControl);
        /* Make sure PreviewPlayer only notifies MEDIA_PLAYBACK_COMPLETE once for each clip!
         * It happens twice in following scenario.
         * To make the clips in preview storyboard are played and switched smoothly,
         * PreviewController uses two PreviewPlayer instances and one AudioPlayer.
         * The two PreviewPlayer use the same AudioPlayer to play the audio,
         * and change the audio source of the AudioPlayer.
         * If the audio source of current playing clip and next clip are dummy
         * audio source(image or video without audio), it will not change the audio source
         * to avoid the "audio glitch", and keep using the current audio source.
         * When the video of current clip reached the EOS, PreviewPlayer will set EOS flag
         * for video and audio, and it will notify MEDIA_PLAYBACK_COMPLETE.
         * But the audio(dummy audio source) is still playing(for next clip),
         * and when it reached the EOS, and video reached EOS,
         * PreviewPlayer will notify MEDIA_PLAYBACK_COMPLETE again. */
        if (!(mFlags & INFORMED_AV_EOS)) {
            notifyListener_l(MEDIA_PLAYBACK_COMPLETE);
            mFlags |= INFORMED_AV_EOS;
        }
        mFlags |= AT_EOS;
        LOGV("onStreamDone end");
        return;
    }
}


status_t PreviewPlayer::play_l() {

    mFlags &= ~SEEK_PREVIEW;

    if (mFlags & PLAYING) {
        return OK;
    }
    mStartNextPlayer = false;

    if (!(mFlags & PREPARED)) {
        status_t err = prepare_l();

        if (err != OK) {
            return err;
        }
    }

    mFlags |= PLAYING;
    mFlags |= FIRST_FRAME;

    bool deferredAudioSeek = false;

    if (mAudioSource != NULL) {
        if (mAudioPlayer == NULL) {
            if (mAudioSink != NULL) {

                mAudioPlayer = new VideoEditorAudioPlayer(mAudioSink, this);
                mVeAudioPlayer =
                          (VideoEditorAudioPlayer*)mAudioPlayer;

                mAudioPlayer->setSource(mAudioSource);

                mVeAudioPlayer->setAudioMixSettings(
                 mPreviewPlayerAudioMixSettings);

                mVeAudioPlayer->setAudioMixPCMFileHandle(
                 mAudioMixPCMFileHandle);

                mVeAudioPlayer->setAudioMixStoryBoardSkimTimeStamp(
                 mAudioMixStoryBoardTS, mCurrentMediaBeginCutTime,
                 mCurrentMediaVolumeValue);

                 mFlags |= AUDIOPLAYER_STARTED;
                // We've already started the MediaSource in order to enable
                // the prefetcher to read its data.
                status_t err = mVeAudioPlayer->start(
                        true /* sourceAlreadyStarted */);

                if (err != OK) {
                    //delete mAudioPlayer;
                    mAudioPlayer = NULL;

                    mFlags &= ~(PLAYING | FIRST_FRAME);
                    return err;
                }

                mTimeSource = mVeAudioPlayer;
                mFlags |= AUDIO_RUNNING;
                deferredAudioSeek = true;
                mWatchForAudioSeekComplete = false;
                mWatchForAudioEOS = true;
            }
        } else {
            mVeAudioPlayer = (VideoEditorAudioPlayer*)mAudioPlayer;
            bool isAudioPlayerStarted = mVeAudioPlayer->isStarted();

            if (mIsChangeSourceRequired == true) {
                LOGV("play_l: Change audio source required");

                if (isAudioPlayerStarted == true) {
                    mVeAudioPlayer->pause();
                }

                mVeAudioPlayer->setSource(mAudioSource);
                mVeAudioPlayer->setObserver(this);

                mVeAudioPlayer->setAudioMixSettings(
                 mPreviewPlayerAudioMixSettings);

                mVeAudioPlayer->setAudioMixStoryBoardSkimTimeStamp(
                    mAudioMixStoryBoardTS, mCurrentMediaBeginCutTime,
                    mCurrentMediaVolumeValue);

                if (isAudioPlayerStarted == true) {
                    mVeAudioPlayer->resume();
                } else {
                    status_t err = OK;
                    err = mVeAudioPlayer->start(true);
                    if (err != OK) {
                        mAudioPlayer = NULL;
                        mVeAudioPlayer = NULL;

                        mFlags &= ~(PLAYING | FIRST_FRAME);
                        return err;
                    }
                }
            } else {
                LOGV("play_l: No Source change required");
                mVeAudioPlayer->setAudioMixStoryBoardSkimTimeStamp(
                    mAudioMixStoryBoardTS, mCurrentMediaBeginCutTime,
                    mCurrentMediaVolumeValue);

                mVeAudioPlayer->resume();
            }

            mFlags |= AUDIOPLAYER_STARTED;
            mFlags |= AUDIO_RUNNING;
            mTimeSource = mVeAudioPlayer;
            deferredAudioSeek = true;
            mWatchForAudioSeekComplete = false;
            mWatchForAudioEOS = true;
        }
    }

    if (mTimeSource == NULL && mAudioPlayer == NULL) {
        mTimeSource = &mSystemTimeSource;
    }

    // Set the seek option for Image source files and read.
    // This resets the timestamping for image play
    if (mIsVideoSourceJpg) {
        MediaSource::ReadOptions options;
        MediaBuffer *aLocalBuffer;
        options.setSeekTo(mSeekTimeUs);
        mVideoSource->read(&aLocalBuffer, &options);
        aLocalBuffer->release();
    }

    if (mVideoSource != NULL) {
        // Kick off video playback
        postVideoEvent_l();
    }

    if (deferredAudioSeek) {
        // If there was a seek request while we were paused
        // and we're just starting up again, honor the request now.
        seekAudioIfNecessary_l();
    }

    if (mFlags & AT_EOS) {
        // Legacy behaviour, if a stream finishes playing and then
        // is started again, we play from the start...
        seekTo_l(0);
    }

    return OK;
}


status_t PreviewPlayer::initRenderer_l() {
    if (mSurface != NULL) {
        if(mVideoRenderer == NULL) {
            mVideoRenderer = mNativeWindowRenderer->createRenderInput();
            if (mVideoSource != NULL) {
                updateSizeToRender(mVideoSource->getFormat());
            }
        }
    }
    return OK;
}


status_t PreviewPlayer::seekTo(int64_t timeUs) {

    if ((mExtractorFlags & MediaExtractor::CAN_SEEK) || (mIsVideoSourceJpg)) {
        Mutex::Autolock autoLock(mLock);
        return seekTo_l(timeUs);
    }

    return OK;
}


status_t PreviewPlayer::getVideoDimensions(
        int32_t *width, int32_t *height) const {
    Mutex::Autolock autoLock(mLock);

    if (mVideoWidth < 0 || mVideoHeight < 0) {
        return UNKNOWN_ERROR;
    }

    *width = mVideoWidth;
    *height = mVideoHeight;

    return OK;
}


status_t PreviewPlayer::initAudioDecoder() {
    sp<MetaData> meta = mAudioTrack->getFormat();
    const char *mime;
    CHECK(meta->findCString(kKeyMIMEType, &mime));

    if (!strcasecmp(mime, MEDIA_MIMETYPE_AUDIO_RAW)) {
        mAudioSource = mAudioTrack;
    } else {
        sp<MediaSource> aRawSource;
        aRawSource = OMXCodec::Create(
                mClient.interface(), mAudioTrack->getFormat(),
                false, // createEncoder
                mAudioTrack);

        if(aRawSource != NULL) {
            LOGV("initAudioDecoder: new VideoEditorSRC");
            mAudioSource = new VideoEditorSRC(aRawSource);
        }
    }

    if (mAudioSource != NULL) {
        int64_t durationUs;
        if (mAudioTrack->getFormat()->findInt64(kKeyDuration, &durationUs)) {
            Mutex::Autolock autoLock(mMiscStateLock);
            if (mDurationUs < 0 || durationUs > mDurationUs) {
                mDurationUs = durationUs;
            }
        }
        status_t err = mAudioSource->start();

        if (err != OK) {
            mAudioSource.clear();
            return err;
        }
    } else if (!strcasecmp(mime, MEDIA_MIMETYPE_AUDIO_QCELP)) {
        // For legacy reasons we're simply going to ignore the absence
        // of an audio decoder for QCELP instead of aborting playback
        // altogether.
        return OK;
    }

    return mAudioSource != NULL ? OK : UNKNOWN_ERROR;
}


status_t PreviewPlayer::initVideoDecoder(uint32_t flags) {

    initRenderer_l();

    if (mVideoRenderer == NULL) {
        LOGE("Cannot create renderer");
        return UNKNOWN_ERROR;
    }

    mVideoSource = OMXCodec::Create(
            mClient.interface(), mVideoTrack->getFormat(),
            false,
            mVideoTrack,
            NULL, flags, mVideoRenderer->getTargetWindow());

    if (mVideoSource != NULL) {
        int64_t durationUs;
        if (mVideoTrack->getFormat()->findInt64(kKeyDuration, &durationUs)) {
            Mutex::Autolock autoLock(mMiscStateLock);
            if (mDurationUs < 0 || durationUs > mDurationUs) {
                mDurationUs = durationUs;
            }
        }

        updateSizeToRender(mVideoTrack->getFormat());

        status_t err = mVideoSource->start();

        if (err != OK) {
            mVideoSource.clear();
            return err;
        }
    }

    return mVideoSource != NULL ? OK : UNKNOWN_ERROR;
}


void PreviewPlayer::onVideoEvent() {
    uint32_t i=0;
    M4OSA_ERR err1 = M4NO_ERROR;
    int64_t imageFrameTimeUs = 0;

    Mutex::Autolock autoLock(mLock);
    if (!mVideoEventPending) {
        // The event has been cancelled in reset_l() but had already
        // been scheduled for execution at that time.
        return;
    }
    mVideoEventPending = false;

    if (mFlags & SEEK_PREVIEW) {
        mFlags &= ~SEEK_PREVIEW;
        return;
    }

    TimeSource *ts_st =  &mSystemTimeSource;
    int64_t timeStartUs = ts_st->getRealTimeUs();

    if (mSeeking != NO_SEEK) {

        if(mAudioSource != NULL) {

            // We're going to seek the video source first, followed by
            // the audio source.
            // In order to avoid jumps in the DataSource offset caused by
            // the audio codec prefetching data from the old locations
            // while the video codec is already reading data from the new
            // locations, we'll "pause" the audio source, causing it to
            // stop reading input data until a subsequent seek.

            if (mAudioPlayer != NULL && (mFlags & AUDIO_RUNNING)) {
                mAudioPlayer->pause();
                mFlags &= ~AUDIO_RUNNING;
            }
            mAudioSource->pause();
        }
    }

    if (!mVideoBuffer) {
        MediaSource::ReadOptions options;
        if (mSeeking != NO_SEEK) {
            LOGV("LV PLAYER seeking to %lld us (%.2f secs)", mSeekTimeUs,
                                                      mSeekTimeUs / 1E6);

            options.setSeekTo(
                    mSeekTimeUs, MediaSource::ReadOptions::SEEK_CLOSEST);
        }
        for (;;) {
            status_t err = mVideoSource->read(&mVideoBuffer, &options);
            options.clearSeekTo();

            if (err != OK) {
                CHECK_EQ(mVideoBuffer, NULL);

                if (err == INFO_FORMAT_CHANGED) {
                    LOGV("LV PLAYER VideoSource signalled format change");
                    notifyVideoSize_l();

                    if (mVideoRenderer != NULL) {
                        mVideoRendererIsPreview = false;
                        err = initRenderer_l();
                        if (err != OK) {
                            postStreamDoneEvent_l(err);
                        }

                    }

                    updateSizeToRender(mVideoSource->getFormat());
                    continue;
                }
                // So video playback is complete, but we may still have
                // a seek request pending that needs to be applied to the audio track
                if (mSeeking != NO_SEEK) {
                    LOGV("video stream ended while seeking!");
                }
                finishSeekIfNecessary(-1);
                LOGV("PreviewPlayer: onVideoEvent EOS reached.");
                mFlags |= VIDEO_AT_EOS;
                mFlags |= AUDIO_AT_EOS;
                mOverlayUpdateEventPosted = false;
                postStreamDoneEvent_l(err);
                // Set the last decoded timestamp to duration
                mDecodedVideoTs = (mPlayEndTimeMsec*1000LL);
                return;
            }

            if (mVideoBuffer->range_length() == 0) {
                // Some decoders, notably the PV AVC software decoder
                // return spurious empty buffers that we just want to ignore.

                mVideoBuffer->release();
                mVideoBuffer = NULL;
                continue;
            }

            int64_t videoTimeUs;
            CHECK(mVideoBuffer->meta_data()->findInt64(kKeyTime, &videoTimeUs));

            if (mSeeking != NO_SEEK) {
                if (videoTimeUs < mSeekTimeUs) {
                    // buffers are before seek time
                    // ignore them
                    mVideoBuffer->release();
                    mVideoBuffer = NULL;
                    continue;
                }
            } else {
                if((videoTimeUs/1000) < mPlayBeginTimeMsec) {
                    // Frames are before begin cut time
                    // Donot render
                    mVideoBuffer->release();
                    mVideoBuffer = NULL;
                    continue;
                }
            }
            break;
        }
    }

    mNumberDecVideoFrames++;

    int64_t timeUs;
    CHECK(mVideoBuffer->meta_data()->findInt64(kKeyTime, &timeUs));

    {
        Mutex::Autolock autoLock(mMiscStateLock);
        mVideoTimeUs = timeUs;
    }


    if(!mStartNextPlayer) {
        int64_t playbackTimeRemaining = (mPlayEndTimeMsec*1000LL) - timeUs;
        if(playbackTimeRemaining <= 1500000) {
            //When less than 1.5 sec of playback left
            // send notification to start next player

            mStartNextPlayer = true;
            notifyListener_l(0xAAAAAAAA);
        }
    }

    SeekType wasSeeking = mSeeking;
    finishSeekIfNecessary(timeUs);
    if (mAudioPlayer != NULL && !(mFlags & (AUDIO_RUNNING))) {
        status_t err = startAudioPlayer_l();
        if (err != OK) {
            LOGE("Starting the audio player failed w/ err %d", err);
            return;
        }
    }

    TimeSource *ts = (mFlags & AUDIO_AT_EOS) ? &mSystemTimeSource : mTimeSource;

    if(ts == NULL) {
        mVideoBuffer->release();
        mVideoBuffer = NULL;
        return;
    }

    if(!mIsVideoSourceJpg) {
        if (mFlags & FIRST_FRAME) {
            mFlags &= ~FIRST_FRAME;

            mTimeSourceDeltaUs = ts->getRealTimeUs() - timeUs;
        }

        int64_t realTimeUs, mediaTimeUs;
        if (!(mFlags & AUDIO_AT_EOS) && mAudioPlayer != NULL
            && mAudioPlayer->getMediaTimeMapping(&realTimeUs, &mediaTimeUs)) {
            mTimeSourceDeltaUs = realTimeUs - mediaTimeUs;
        }

        int64_t nowUs = ts->getRealTimeUs() - mTimeSourceDeltaUs;

        int64_t latenessUs = nowUs - timeUs;

        if (wasSeeking != NO_SEEK) {
            // Let's display the first frame after seeking right away.
            latenessUs = 0;
        }
        LOGV("Audio time stamp = %lld and video time stamp = %lld",
                                            ts->getRealTimeUs(),timeUs);
        if (latenessUs > 40000) {
            // We're more than 40ms late.

            LOGV("LV PLAYER we're late by %lld us (%.2f secs)",
                                           latenessUs, latenessUs / 1E6);

            mVideoBuffer->release();
            mVideoBuffer = NULL;
            postVideoEvent_l(0);
            return;
        }

        if (latenessUs < -25000) {
            // We're more than 25ms early.
            LOGV("We're more than 25ms early, lateness %lld", latenessUs);

            postVideoEvent_l(25000);
            return;
        }
    }

    if (mVideoRendererIsPreview || mVideoRenderer == NULL) {
        mVideoRendererIsPreview = false;

        status_t err = initRenderer_l();
        if (err != OK) {
            postStreamDoneEvent_l(err);
        }
    }

    // If timestamp exceeds endCutTime of clip, donot render
    if((timeUs/1000) > mPlayEndTimeMsec) {
        mVideoBuffer->release();
        mVideoBuffer = NULL;
        mFlags |= VIDEO_AT_EOS;
        mFlags |= AUDIO_AT_EOS;
        LOGV("PreviewPlayer: onVideoEvent timeUs > mPlayEndTime; send EOS..");
        mOverlayUpdateEventPosted = false;
        // Set the last decoded timestamp to duration
        mDecodedVideoTs = (mPlayEndTimeMsec*1000LL);
        postStreamDoneEvent_l(ERROR_END_OF_STREAM);
        return;
    }
    // Capture the frame timestamp to be rendered
    mDecodedVideoTs = timeUs;

    // Post processing to apply video effects
    for(i=0;i<mNumberEffects;i++) {
        // First check if effect starttime matches the clip being previewed
        if((mEffectsSettings[i].uiStartTime < (mDecVideoTsStoryBoard/1000)) ||
        (mEffectsSettings[i].uiStartTime >=
         ((mDecVideoTsStoryBoard/1000) + mPlayEndTimeMsec - mPlayBeginTimeMsec)))
        {
            // This effect doesn't belong to this clip, check next one
            continue;
        }
        // Check if effect applies to this particular frame timestamp
        if((mEffectsSettings[i].uiStartTime <=
         (((timeUs+mDecVideoTsStoryBoard)/1000)-mPlayBeginTimeMsec)) &&
            ((mEffectsSettings[i].uiStartTime+mEffectsSettings[i].uiDuration) >=
             (((timeUs+mDecVideoTsStoryBoard)/1000)-mPlayBeginTimeMsec))
              && (mEffectsSettings[i].uiDuration != 0)) {
            setVideoPostProcessingNode(
             mEffectsSettings[i].VideoEffectType, TRUE);
        }
        else {
            setVideoPostProcessingNode(
             mEffectsSettings[i].VideoEffectType, FALSE);
        }
    }

    //Provide the overlay Update indication when there is an overlay effect
    if (mCurrentVideoEffect & VIDEO_EFFECT_FRAMING) {
        mCurrentVideoEffect &= ~VIDEO_EFFECT_FRAMING; //never apply framing here.
        if (!mOverlayUpdateEventPosted) {
            // Find the effect in effectSettings array
            M4OSA_UInt32 index;
            for (index = 0; index < mNumberEffects; index++) {
                M4OSA_UInt32 timeMs = mDecodedVideoTs/1000;
                M4OSA_UInt32 timeOffset = mDecVideoTsStoryBoard/1000;
                if(mEffectsSettings[index].VideoEffectType ==
                    (M4VSS3GPP_VideoEffectType)M4xVSS_kVideoEffectType_Framing) {
                    if (((mEffectsSettings[index].uiStartTime + 1) <=
                        timeMs + timeOffset - mPlayBeginTimeMsec) &&
                        ((mEffectsSettings[index].uiStartTime - 1 +
                        mEffectsSettings[index].uiDuration) >=
                        timeMs + timeOffset - mPlayBeginTimeMsec))
                    {
                        break;
                    }
                }
            }
            if (index < mNumberEffects) {
                mCurrFramingEffectIndex = index;
                mOverlayUpdateEventPosted = true;
                postOverlayUpdateEvent_l();
                LOGV("Framing index = %d", mCurrFramingEffectIndex);
            } else {
                LOGV("No framing effects found");
            }
        }

    } else if (mOverlayUpdateEventPosted) {
        //Post the event when the overlay is no more valid
        LOGV("Overlay is Done");
        mOverlayUpdateEventPosted = false;
        postOverlayUpdateEvent_l();
    }

    if (mVideoRenderer != NULL) {
        mVideoRenderer->render(mVideoBuffer, mCurrentVideoEffect,
                mRenderingMode, mIsVideoSourceJpg);
    }

    mVideoBuffer->release();
    mVideoBuffer = NULL;

    // Post progress callback based on callback interval set
    if(mNumberDecVideoFrames >= mProgressCbInterval) {
        postProgressCallbackEvent_l();
        mNumberDecVideoFrames = 0;  // reset counter
    }

    // if reached EndCutTime of clip, post EOS event
    if((timeUs/1000) >= mPlayEndTimeMsec) {
        LOGV("PreviewPlayer: onVideoEvent EOS.");
        mFlags |= VIDEO_AT_EOS;
        mFlags |= AUDIO_AT_EOS;
        mOverlayUpdateEventPosted = false;
        // Set the last decoded timestamp to duration
        mDecodedVideoTs = (mPlayEndTimeMsec*1000LL);
        postStreamDoneEvent_l(ERROR_END_OF_STREAM);
    }
    else {
        if ((wasSeeking != NO_SEEK) && (mFlags & SEEK_PREVIEW)) {
            mFlags &= ~SEEK_PREVIEW;
            return;
        }

        if(!mIsVideoSourceJpg) {
            postVideoEvent_l(0);
        }
        else {
            postVideoEvent_l(33000);
        }
    }
}

status_t PreviewPlayer::prepare() {
    Mutex::Autolock autoLock(mLock);
    return prepare_l();
}

status_t PreviewPlayer::prepare_l() {
    if (mFlags & PREPARED) {
        return OK;
    }

    if (mFlags & PREPARING) {
        return UNKNOWN_ERROR;
    }

    mIsAsyncPrepare = false;
    status_t err = prepareAsync_l();

    if (err != OK) {
        return err;
    }

    while (mFlags & PREPARING) {
        mPreparedCondition.wait(mLock);
    }

    return mPrepareResult;
}

status_t PreviewPlayer::prepareAsync_l() {
    if (mFlags & PREPARING) {
        return UNKNOWN_ERROR;  // async prepare already pending
    }

    if (!mQueueStarted) {
        mQueue.start();
        mQueueStarted = true;
    }

    mFlags |= PREPARING;
    mAsyncPrepareEvent = new PreviewPlayerEvent(
            this, &PreviewPlayer::onPrepareAsyncEvent);

    mQueue.postEvent(mAsyncPrepareEvent);

    return OK;
}

status_t PreviewPlayer::finishSetDataSource_l() {
    sp<DataSource> dataSource;
    sp<MediaExtractor> extractor;

    dataSource = DataSource::CreateFromURI(mUri.string(), &mUriHeaders);

    if (dataSource == NULL) {
        return UNKNOWN_ERROR;
    }

    //If file type is .rgb, then no need to check for Extractor
    int uriLen = strlen(mUri);
    int startOffset = uriLen - 4;
    if(!strncasecmp(mUri+startOffset, ".rgb", 4)) {
        extractor = NULL;
    }
    else {
        extractor = MediaExtractor::Create(dataSource,
                                        MEDIA_MIMETYPE_CONTAINER_MPEG4);
    }

    if (extractor == NULL) {
        LOGV("PreviewPlayer::finishSetDataSource_l  extractor == NULL");
        return setDataSource_l_jpg();
    }

    return setDataSource_l(extractor);
}


// static
bool PreviewPlayer::ContinuePreparation(void *cookie) {
    PreviewPlayer *me = static_cast<PreviewPlayer *>(cookie);

    return (me->mFlags & PREPARE_CANCELLED) == 0;
}

void PreviewPlayer::onPrepareAsyncEvent() {
    Mutex::Autolock autoLock(mLock);
    LOGV("onPrepareAsyncEvent");

    if (mFlags & PREPARE_CANCELLED) {
        LOGV("LV PLAYER prepare was cancelled before doing anything");
        abortPrepare(UNKNOWN_ERROR);
        return;
    }

    if (mUri.size() > 0) {
        status_t err = finishSetDataSource_l();

        if (err != OK) {
            abortPrepare(err);
            return;
        }
    }

    if (mVideoTrack != NULL && mVideoSource == NULL) {
        status_t err = initVideoDecoder(OMXCodec::kHardwareCodecsOnly);

        if (err != OK) {
            abortPrepare(err);
            return;
        }
    }

    if (mAudioTrack != NULL && mAudioSource == NULL) {
        status_t err = initAudioDecoder();

        if (err != OK) {
            abortPrepare(err);
            return;
        }
    }
    finishAsyncPrepare_l();

}

void PreviewPlayer::finishAsyncPrepare_l() {
    if (mIsAsyncPrepare) {
        if (mVideoSource == NULL) {
            LOGV("finishAsyncPrepare_l: MEDIA_SET_VIDEO_SIZE 0 0 ");
            notifyListener_l(MEDIA_SET_VIDEO_SIZE, 0, 0);
        } else {
            LOGV("finishAsyncPrepare_l: MEDIA_SET_VIDEO_SIZE");
            notifyVideoSize_l();
        }
        LOGV("finishAsyncPrepare_l: MEDIA_PREPARED");
        notifyListener_l(MEDIA_PREPARED);
    }

    mPrepareResult = OK;
    mFlags &= ~(PREPARING|PREPARE_CANCELLED);
    mFlags |= PREPARED;
    mAsyncPrepareEvent = NULL;
    mPreparedCondition.broadcast();
}

void PreviewPlayer::acquireLock() {
    LOGV("acquireLock");
    mLockControl.lock();
}

void PreviewPlayer::releaseLock() {
    LOGV("releaseLock");
    mLockControl.unlock();
}

status_t PreviewPlayer::loadEffectsSettings(
                    M4VSS3GPP_EffectSettings* pEffectSettings, int nEffects) {
    M4OSA_UInt32 i = 0, rgbSize = 0;
    M4VIFI_UInt8 *tmp = M4OSA_NULL;

    mNumberEffects = nEffects;
    mEffectsSettings = pEffectSettings;
    return OK;
}

status_t PreviewPlayer::loadAudioMixSettings(
                    M4xVSS_AudioMixingSettings* pAudioMixSettings) {

    LOGV("PreviewPlayer: loadAudioMixSettings: ");
    mPreviewPlayerAudioMixSettings = pAudioMixSettings;
    return OK;
}

status_t PreviewPlayer::setAudioMixPCMFileHandle(
                    M4OSA_Context pAudioMixPCMFileHandle) {

    LOGV("PreviewPlayer: setAudioMixPCMFileHandle: ");
    mAudioMixPCMFileHandle = pAudioMixPCMFileHandle;
    return OK;
}

status_t PreviewPlayer::setAudioMixStoryBoardParam(
                    M4OSA_UInt32 audioMixStoryBoardTS,
                    M4OSA_UInt32 currentMediaBeginCutTime,
                    M4OSA_UInt32 primaryTrackVolValue ) {

    mAudioMixStoryBoardTS = audioMixStoryBoardTS;
    mCurrentMediaBeginCutTime = currentMediaBeginCutTime;
    mCurrentMediaVolumeValue = primaryTrackVolValue;
    return OK;
}

status_t PreviewPlayer::setPlaybackBeginTime(uint32_t msec) {

    mPlayBeginTimeMsec = msec;
    return OK;
}

status_t PreviewPlayer::setPlaybackEndTime(uint32_t msec) {

    mPlayEndTimeMsec = msec;
    return OK;
}

status_t PreviewPlayer::setStoryboardStartTime(uint32_t msec) {

    mStoryboardStartTimeMsec = msec;
    mDecVideoTsStoryBoard = mStoryboardStartTimeMsec*1000LL;
    return OK;
}

status_t PreviewPlayer::setProgressCallbackInterval(uint32_t cbInterval) {

    mProgressCbInterval = cbInterval;
    return OK;
}


status_t PreviewPlayer::setMediaRenderingMode(
        M4xVSS_MediaRendering mode,
        M4VIDEOEDITING_VideoFrameSize outputVideoSize) {

    mRenderingMode = mode;

    status_t err = OK;
    /* get the video width and height by resolution */
    err = getVideoSizeByResolution(outputVideoSize,
              &mOutputVideoWidth, &mOutputVideoHeight);

    return err;
}

status_t PreviewPlayer::resetJniCallbackTimeStamp() {

    mDecVideoTsStoryBoard = mStoryboardStartTimeMsec*1000LL;
    return OK;
}

void PreviewPlayer::postProgressCallbackEvent_l() {
    if (mProgressCbEventPending) {
        return;
    }
    mProgressCbEventPending = true;

    mQueue.postEvent(mProgressCbEvent);
}


void PreviewPlayer::onProgressCbEvent() {
    Mutex::Autolock autoLock(mLock);
    if (!mProgressCbEventPending) {
        return;
    }
    mProgressCbEventPending = false;
    // If playback starts from previous I-frame,
    // then send frame storyboard duration
    if((mDecodedVideoTs/1000) < mPlayBeginTimeMsec) {
        notifyListener_l(MEDIA_INFO, 0, mDecVideoTsStoryBoard/1000);
    }
    else {
        notifyListener_l(MEDIA_INFO, 0,
        (((mDecodedVideoTs+mDecVideoTsStoryBoard)/1000)-mPlayBeginTimeMsec));
    }
}

void PreviewPlayer::postOverlayUpdateEvent_l() {
    if (mOverlayUpdateEventPending) {
        return;
    }
    mOverlayUpdateEventPending = true;
    mQueue.postEvent(mOverlayUpdateEvent);
}

void PreviewPlayer::onUpdateOverlayEvent() {
    Mutex::Autolock autoLock(mLock);

    if (!mOverlayUpdateEventPending) {
        return;
    }
    mOverlayUpdateEventPending = false;

    int updateState;
    if (mOverlayUpdateEventPosted) {
        updateState = 1;
    } else {
        updateState = 0;
    }
    notifyListener_l(0xBBBBBBBB, updateState, mCurrFramingEffectIndex);
}


void PreviewPlayer::setVideoPostProcessingNode(
                    M4VSS3GPP_VideoEffectType type, M4OSA_Bool enable) {

    uint32_t effect = VIDEO_EFFECT_NONE;

    //Map M4VSS3GPP_VideoEffectType to local enum
    switch(type) {
        case M4VSS3GPP_kVideoEffectType_FadeFromBlack:
            effect = VIDEO_EFFECT_FADEFROMBLACK;
            break;

        case M4VSS3GPP_kVideoEffectType_FadeToBlack:
            effect = VIDEO_EFFECT_FADETOBLACK;
            break;

        case M4xVSS_kVideoEffectType_BlackAndWhite:
            effect = VIDEO_EFFECT_BLACKANDWHITE;
            break;

        case M4xVSS_kVideoEffectType_Pink:
            effect = VIDEO_EFFECT_PINK;
            break;

        case M4xVSS_kVideoEffectType_Green:
            effect = VIDEO_EFFECT_GREEN;
            break;

        case M4xVSS_kVideoEffectType_Sepia:
            effect = VIDEO_EFFECT_SEPIA;
            break;

        case M4xVSS_kVideoEffectType_Negative:
            effect = VIDEO_EFFECT_NEGATIVE;
            break;

        case M4xVSS_kVideoEffectType_Framing:
            effect = VIDEO_EFFECT_FRAMING;
            break;

        case M4xVSS_kVideoEffectType_Fifties:
            effect = VIDEO_EFFECT_FIFTIES;
            break;

        case M4xVSS_kVideoEffectType_ColorRGB16:
            effect = VIDEO_EFFECT_COLOR_RGB16;
            break;

        case M4xVSS_kVideoEffectType_Gradient:
            effect = VIDEO_EFFECT_GRADIENT;
            break;

        default:
            effect = VIDEO_EFFECT_NONE;
            break;
    }

    if(enable == M4OSA_TRUE) {
        //If already set, then no need to set again
        if(!(mCurrentVideoEffect & effect)) {
            mCurrentVideoEffect |= effect;
            if(effect == VIDEO_EFFECT_FIFTIES) {
                mIsFiftiesEffectStarted = true;
            }
        }
    }
    else  {
        //Reset only if already set
        if(mCurrentVideoEffect & effect) {
            mCurrentVideoEffect &= ~effect;
        }
    }
}

status_t PreviewPlayer::setImageClipProperties(uint32_t width,uint32_t height) {
    mVideoWidth = width;
    mVideoHeight = height;
    return OK;
}

status_t PreviewPlayer::readFirstVideoFrame() {
    LOGV("PreviewPlayer::readFirstVideoFrame");

    if (!mVideoBuffer) {
        MediaSource::ReadOptions options;
        if (mSeeking != NO_SEEK) {
            LOGV("LV PLAYER seeking to %lld us (%.2f secs)", mSeekTimeUs,
                    mSeekTimeUs / 1E6);

            options.setSeekTo(
                    mSeekTimeUs, MediaSource::ReadOptions::SEEK_CLOSEST);
        }
        for (;;) {
            status_t err = mVideoSource->read(&mVideoBuffer, &options);
            options.clearSeekTo();

            if (err != OK) {
                CHECK_EQ(mVideoBuffer, NULL);

                if (err == INFO_FORMAT_CHANGED) {
                    LOGV("LV PLAYER VideoSource signalled format change");
                    notifyVideoSize_l();

                    if (mVideoRenderer != NULL) {
                        mVideoRendererIsPreview = false;
                        err = initRenderer_l();
                        if (err != OK) {
                            postStreamDoneEvent_l(err);
                        }
                    }

                    updateSizeToRender(mVideoSource->getFormat());
                    continue;
                }
                LOGV("PreviewPlayer: onVideoEvent EOS reached.");
                mFlags |= VIDEO_AT_EOS;
                mFlags |= AUDIO_AT_EOS;
                postStreamDoneEvent_l(err);
                return OK;
            }

            if (mVideoBuffer->range_length() == 0) {
                // Some decoders, notably the PV AVC software decoder
                // return spurious empty buffers that we just want to ignore.

                mVideoBuffer->release();
                mVideoBuffer = NULL;
                continue;
            }

            int64_t videoTimeUs;
            CHECK(mVideoBuffer->meta_data()->findInt64(kKeyTime, &videoTimeUs));
            if (mSeeking != NO_SEEK) {
                if (videoTimeUs < mSeekTimeUs) {
                    // buffers are before seek time
                    // ignore them
                    mVideoBuffer->release();
                    mVideoBuffer = NULL;
                    continue;
                }
            } else {
                if((videoTimeUs/1000) < mPlayBeginTimeMsec) {
                    // buffers are before begin cut time
                    // ignore them
                    mVideoBuffer->release();
                    mVideoBuffer = NULL;
                    continue;
                }
            }
            break;
        }
    }

    int64_t timeUs;
    CHECK(mVideoBuffer->meta_data()->findInt64(kKeyTime, &timeUs));

    {
        Mutex::Autolock autoLock(mMiscStateLock);
        mVideoTimeUs = timeUs;
    }

    mDecodedVideoTs = timeUs;

    return OK;

}

status_t PreviewPlayer::getLastRenderedTimeMs(uint32_t *lastRenderedTimeMs) {
    *lastRenderedTimeMs = (((mDecodedVideoTs+mDecVideoTsStoryBoard)/1000)-mPlayBeginTimeMsec);
    return OK;
}

void PreviewPlayer::updateSizeToRender(sp<MetaData> meta) {
    if (mVideoRenderer) {
        mVideoRenderer->updateVideoSize(meta);
    }
}

}  // namespace android
