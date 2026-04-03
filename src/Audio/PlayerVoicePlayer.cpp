#include "Audio/PlayerVoicePlayer.hpp"
#include "Audio/VoiceManager.hpp"

#include "UnityEngine/Time.hpp"

DEFINE_TYPE(MultiplayerChat::Audio, PlayerVoicePlayer);

namespace MultiplayerChat::Audio {
    void PlayerVoicePlayer::ctor(StringW playerUserId, int jitterBufferMs, float spatialBlend) {
        INVOKE_CTOR();

        _playerUserId = playerUserId;
        _isPlaying = false;
        _jitterBufferMs = jitterBufferMs;

        _spatialBlend = spatialBlend;
        _audioSource = nullptr;
        _audioClip = UnityEngine::AudioClip::Create("JitterBufferClip", get_clipSampleSize(), (int)VoiceManager::OpusChannels, (int)VoiceManager::DecodeFrequency, false);
        _playbackBuffer = ArrayW<float>(il2cpp_array_size_t(get_clipFeedSize()));

        StopImmediate();
    }

    void PlayerVoicePlayer::StopImmediate() {
        _audioClip->SetData(get_emptyClipSamples(), 0);
        if (_audioSource && _audioSource->m_CachedPtr.m_value) {
            _audioSource->set_loop(false);
            _audioSource->set_timeSamples(0);
            _audioSource->set_volume(0.0f);
            _audioSource->Stop();
        }

        if (get_isPlaying()) {
            _isPlaying = false;
            stopPlaybackEvent.invoke(this);
        }

        _streamBuffer.Flush();

        _havePendingFragments = false;
        _isJitterBuffering = false;
        _isWritingBuffer = false;
        _lastPlaybackPos = 0;
        _playbackIterations = 0;
        _bufferPos = 0;
        _bufferIterations = 0;
        _transmissionEnding = false;
        _transmissionEnded = true;
        _deadFrames = 0;
    }

    void PlayerVoicePlayer::Dispose() {
        _streamBuffer.Close();
        if (_audioClip && _audioClip->m_CachedPtr.m_value)
            UnityEngine::Object::Destroy(_audioClip);
        _isPlaying = false;
    }

    void PlayerVoicePlayer::FeedFragment(ArrayW<float> decodeBuffer, int decodedLength) {
        if (decodedLength <= 0) {
            _transmissionEnding = true;
            return;
        }

        _streamBuffer.Write(decodeBuffer, 0, decodedLength);

        _havePendingFragments = true;

        if (_transmissionEnded) {
            _transmissionEnding = false;
            _transmissionEnded = false;
        }
    }

    void PlayerVoicePlayer::ConfigureAudioSource(UnityEngine::AudioSource* audioSource) {
        _audioSource = reinterpret_cast<ExtendedAudioSource*>(audioSource);
        _audioSource->set_clip(_audioClip);
        _audioSource->set_timeSamples(0);
        _audioSource->set_volume(0.0f);

        if (_spatialBlend <= 0) {
            _audioSource->set_spatialize(false);
            _audioSource->set_spatialBlend(0.0f);
        } else {
            _audioSource->set_spatialize(true);
            _audioSource->set_spatialBlend(_spatialBlend);
        }
    }

    void PlayerVoicePlayer::SetMultiplayerAvatarAudioController(BeatSaber::AvatarCore::MultiplayerAvatarAudioController* avatarAudio) {
        ConfigureAudioSource(avatarAudio->_audioSource);
    }

    void PlayerVoicePlayer::StartPlayback() {
        if (!_audioSource || !_audioSource->m_CachedPtr.m_value || get_isPlaying()) return;

        _audioSource->set_timeSamples(0);
        _audioSource->set_loop(true);
        _audioSource->set_volume(1.0f);

        _audioSource->Play();
        _isPlaying = true;
        startPlaybackEvent.invoke(this);
    }

    void PlayerVoicePlayer::Update() {
        if (!_audioSource || !_audioSource->m_CachedPtr.m_value) {
            if (_havePendingFragments || _isWritingBuffer)
                StopImmediate();
            return;
        }

        if (_isWritingBuffer)
            UpdateActive();
        else
            UpdateInactive();
    }

    void PlayerVoicePlayer::UpdateInactive() {
        if (!_havePendingFragments) { // no pending fragments, nothing to do
            _isJitterBuffering = false;
            return;
        }

        if (!_isJitterBuffering) { // start jitter buffering
            _jitterStartTime = UnityEngine::Time::get_unscaledTime();
            _isJitterBuffering = true;
            startBufferingEvent.invoke(this);
            return;
        }

        auto jitterTime = UnityEngine::Time::get_unscaledTime() - _jitterStartTime;
        if ((jitterTime * 1000) < _jitterBufferMs) return;

        _isJitterBuffering = false;
        _isWritingBuffer = true;
        _bufferPos = 0;

        StartPlayback();
        UpdateActive(true);
    }

    void PlayerVoicePlayer::UpdateActive(bool firstUpdate) {
        auto playbackPos = _audioSource->get_timeSamples();

        for (int i = 0; i < 3; i++) {
            auto peekSampleCount = _streamBuffer.Peek(_playbackBuffer, 0, get_clipFeedSize());
			
			_bufferPos += peekSampleCount;
            if (_bufferPos >= get_clipSampleSize())
                _bufferIterations++;
            _bufferPos %= get_clipSampleSize();

            if (playbackPos < _lastPlaybackPos)
                _playbackIterations++;
            _lastPlaybackPos = playbackPos;

            if (peekSampleCount == 0) {
                _havePendingFragments = false;
                if (_transmissionEnding) {
                    StopImmediate();
                    return;
                }

                auto absPlaybackPos = GetAbsoluteSamples(_playbackIterations, playbackPos);
                auto absBufferPos = GetAbsoluteSamples(_bufferIterations, _bufferPos);

                if ((absPlaybackPos + get_clipFeedSize()) <= absBufferPos) return;

                // buffer depleted, playback caught up
                // audio will loop, stale samples could be played which we want to avoid

                if (++___backing_field__deadFrames < 5)
                    return;

                StopImmediate();
                return;
            }

            _streamBuffer.Advance(peekSampleCount);

            _audioClip->SetData(_playbackBuffer, _bufferPos);

            _deadFrames = 0;
        }
    }

    int PlayerVoicePlayer::get_clipSampleSize() { return VoiceManager::DecodeFrequency.value__; }

    int PlayerVoicePlayer::get_clipFeedSize() { return VoiceManager::MaxFrameLength; }

    ArrayW<float> PlayerVoicePlayer::get_emptyClipSamples() {
        static SafePtr<Array<float>> emptyClipSamples;
        if (!emptyClipSamples || !emptyClipSamples.ptr()) {
            emptyClipSamples = Array<float>::NewLength(get_clipSampleSize());
        }
        return emptyClipSamples.ptr();
    }
}
