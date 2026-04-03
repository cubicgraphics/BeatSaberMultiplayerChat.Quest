#include "Audio/VoiceManager.hpp"
#include "Audio/MicrophoneManager.hpp"
#include "Audio/AudioGain.hpp"
#include "Audio/AudioResample.hpp"
#include "Core/ChatManager.hpp"

#include "config.hpp"

#include "UnityEngine/GameObject.hpp"

#include "UnityOpus/OpusApplication.hpp"

#include "custom-types/shared/delegate.hpp"

DEFINE_TYPE(MultiplayerChat::Audio, VoiceManager);

template<typename T, typename U, typename V = System::Collections::Generic::Dictionary_2<U, T>*>
static inline bool TryGetValue(V dict, T key, U& out) {
    if (dict->ContainsKey(key)) {
        out = dict->get_Item(key);
        return true;
    }
    return false;
}

namespace MultiplayerChat::Audio {
    void VoiceManager::ctor() {
        INVOKE_CTOR();

        _opusEncoder = nullptr;
        _opusDecoder = UnityOpus::Decoder::New_ctor(DecodeFrequency, OpusChannels);
        _captureFrequency = 0;
        _encodeFrequency = UnityOpus::SamplingFrequency::Frequency_48000;

        _encodeSampleBuffer = ArrayW<float>(il2cpp_array_size_t(MaxFrameLength));
        _resampleBuffer = ArrayW<float>(il2cpp_array_size_t(MaxFrameLength));
        _encodeOutputBuffer = ArrayW<uint8_t>(il2cpp_array_size_t(MaxFrameLength * sizeof(float)));
        _encodeSampleIndex = 0;

        _encodeFrameLength = GetFrameLength((int)UnityOpus::SamplingFrequency::Frequency_48000);

        _decodeSampleBuffer = ArrayW<float>(UnityOpus::Decoder::maximumPacketDuration * (int)OpusChannels);
        _isLoopbackTesting = false;

        _loopbackVoicePlayer = PlayerVoicePlayer::New_ctor("loopback", 250, 0);
        _voicePlayers = VoicePlayerDict::New_ctor();
    }

    void VoiceManager::Inject(MultiplayerCore::Networking::MpPacketSerializer* packetSerializer, GlobalNamespace::IMultiplayerSessionManager* sessionManager, Zenject::DiContainer* container) {
        _chatManager = container->Resolve<Core::ChatManager*>();
        _microphoneManager = container->Resolve<MicrophoneManager*>();
        _packetSerializer = packetSerializer;
        _sessionManager = sessionManager;
    }

    void VoiceManager::Initialize() {
        _microphoneManager->onFragmentReadyEvent += {&VoiceManager::HandleMicrophoneFragment, this};
        _microphoneManager->onCaptureEndEvent += {&VoiceManager::HandleMicrophoneEnd, this};

        _packetSerializer->RegisterCallback<Network::MpcVoicePacket*>(
            std::bind(&VoiceManager::HandleVoicePacket, this, std::placeholders::_1, std::placeholders::_2)
        );

        _disconnectedAction =
            custom_types::MakeDelegate<System::Action_1<GlobalNamespace::DisconnectedReason>*>(
                std::function<void(GlobalNamespace::DisconnectedReason)>(
                    std::bind(&VoiceManager::HandleSessionDisconnected, this, std::placeholders::_1)
                )
            );

        _sessionManager->add_disconnectedEvent(_disconnectedAction);
    }

    void VoiceManager::Tick() {
        _loopbackVoicePlayer->Update();

        auto enumerator = _voicePlayers->GetEnumerator();
        while (enumerator.MoveNext())
            enumerator.get_Current().value->Update();

        enumerator.Dispose();
    }

    void VoiceManager::Dispose() {
        _sessionManager->remove_disconnectedEvent(_disconnectedAction);

        if (_microphoneManager->get_isCapturing())
            _microphoneManager->StopCapture();

        _microphoneManager->onFragmentReadyEvent -= {&VoiceManager::HandleMicrophoneFragment, this};
        _microphoneManager->onCaptureEndEvent -= {&VoiceManager::HandleMicrophoneEnd, this};

        _packetSerializer->UnregisterCallback<Network::MpcVoicePacket*>();

        if (_opusEncoder) _opusEncoder->Dispose();
        _opusDecoder->Dispose();

        _isLoopbackTesting = false;
        if (_loopbackTester && _loopbackTester->m_CachedPtr.m_value)
            UnityEngine::Object::Destroy(_loopbackTester);
    }

    void VoiceManager::HandleSessionDisconnected(GlobalNamespace::DisconnectedReason reason) {
        StopVoiceTransmission();
        StopLoopbackTest();
    }

    int VoiceManager::GetFrameLength(int frequency) {
        return frequency / (1000 / MsPerFrame);
    }

    UnityOpus::SamplingFrequency VoiceManager::GetEncodeFrequency(int inputFrequency) {
        static std::array<UnityOpus::SamplingFrequency, 4> opusFrequencies {
            UnityOpus::SamplingFrequency::Frequency_24000,
            UnityOpus::SamplingFrequency::Frequency_16000,
            UnityOpus::SamplingFrequency::Frequency_12000,
            UnityOpus::SamplingFrequency::Frequency_8000
        };

        // check all the available frequencies starting from 24000.
        auto lastFreq = UnityOpus::SamplingFrequency::Frequency_48000;
        for (auto freq : opusFrequencies) {
            // return the upper bound frequency of our currently checked
            if (inputFrequency > freq.value__)
                return lastFreq;
            lastFreq = freq;
        }

        return UnityOpus::SamplingFrequency::Frequency_8000;
    }

    int VoiceManager::get_encodeFrameLength() const { return _encodeFrameLength; }

    void VoiceManager::EnsureEncoderForCaptureFrequency(int captureFrequency) {
        // if we have no encoder or the frequencies don't match, we should reinitialize
        if (!_opusEncoder || _captureFrequency != captureFrequency) {
            _captureFrequency = captureFrequency;
            _encodeFrequency = GetEncodeFrequency(captureFrequency);

            _encodeFrameLength = GetFrameLength(_encodeFrequency.value__);

            // ensure the encode sample buffer can't be too small
            if (_encodeSampleBuffer.size() < _encodeFrameLength) {
                _encodeSampleBuffer = ArrayW<float>(il2cpp_array_size_t(_encodeFrameLength));
            }

            // dispose of old encoder and make a new one
            if (_opusEncoder) _opusEncoder->Dispose();
            _opusEncoder = UnityOpus::Encoder::New_ctor(
                _encodeFrequency, OpusChannels, UnityOpus::OpusApplication::VoIP
            );
            _opusEncoder->set_Bitrate(Bitrate);
            _opusEncoder->set_Complexity(OpusComplexity);
            _opusEncoder->set_Signal(UnityOpus::OpusSignal::Voice);

            INFO("(Re)Initialized Opus encoder (captureFrequency={}, encodeFrequency={}, encodeFrameLength={})", captureFrequency, _encodeFrequency.value__, get_encodeFrameLength());
        }
    }

    void VoiceManager::EnsureResampleBufferSize(std::size_t minimumSize) {
        if (_resampleBuffer.size() < minimumSize) {
            _resampleBuffer = ArrayW<float>(il2cpp_array_size_t(minimumSize));
        }
    }

    void VoiceManager::HandleMicrophoneFragment(ArrayW<float> samples, int captureFrequency) {
        // Apply Gain
        Audio::AudioGain::Apply(samples, config.microphoneGain);

        // ensure we have the correct encoder for this capture freq
        EnsureEncoderForCaptureFrequency(captureFrequency);

        // if frequency does not match target, resample audio
        auto encodeFrequencyInt = _encodeFrequency.value__;

        float* copySrcBuffer;
        int copySrcLength;
        if (captureFrequency == encodeFrequencyInt) {
            copySrcBuffer = samples.begin();
            copySrcLength = samples.size();
        } else {
            try {
                EnsureResampleBufferSize(Audio::AudioResample::ResampledSampleCount(samples.size(), captureFrequency, encodeFrequencyInt));

                copySrcBuffer = _resampleBuffer.begin();
                copySrcLength = Audio::AudioResample::Resample(samples, _resampleBuffer, captureFrequency, encodeFrequencyInt);
            } catch (std::runtime_error& e) {
                ERROR("Error thrown while resampling buffer: {}", e.what());
                return;
            }
        }

        // Continuously write to encode buffer until it reaches the target frame length, then encode
        for (auto i = 0; i < copySrcLength; i++) {
            _encodeSampleBuffer[_encodeSampleIndex++] = copySrcBuffer[i];
            if (_encodeSampleIndex != get_encodeFrameLength()) continue;

            auto encodedLength = _opusEncoder->Encode(_encodeSampleBuffer, get_encodeFrameLength(), _encodeOutputBuffer);
            HandleEncodedFrame(encodedLength);

            _encodeSampleIndex = 0;
        }
    }

    void VoiceManager::HandleMicrophoneEnd() {
        memset(_encodeSampleBuffer.begin(), 0, _encodeSampleBuffer.size() * sizeof(float));
        memset(_encodeOutputBuffer.begin(), 0, _encodeOutputBuffer.size() * sizeof(uint8_t));
    }

    void VoiceManager::HandleEncodedFrame(int encodedLength) {
        if (encodedLength <= 0) {
            ERROR("Encoded audio length was invalid! ({})", encodedLength);
            return;
        }

        auto voicePacket = Network::MpcVoicePacket::Obtain();

        try {
            voicePacket->AllocatePooledBuffer(encodedLength);
            memcpy(voicePacket->data.begin(), _encodeOutputBuffer.begin(), encodedLength * sizeof(uint8_t));

            if (get_isLoopbackTesting()) {
                HandleVoicePacket(voicePacket, nullptr);
                voicePacket->Release();
                return;
            }

            if (!_sessionManager->get_isConnected() || !_sessionManager->get_isSyncTimeInitialized() || !get_isTransmitting()) {
                voicePacket->Release();
                return;
            }

            _packetSerializer->SendUnreliable(voicePacket);
        } catch(const std::runtime_error& e) {
            ERROR("Error thrown while setting up voice packet: {}", e.what());
        }

        voicePacket->Release();
    }

    void VoiceManager::HandleVoicePacket(Network::MpcVoicePacket* packet, GlobalNamespace::IConnectedPlayer* source) {
        if (!config.enableVoiceChat) return;

        try {
            auto dataLength = packet->get_dataLength();
            if (dataLength > 0)
                HandleVoiceFragment(_opusDecoder->Decode(packet->data, dataLength, _decodeSampleBuffer, 0), source);
            else
                HandleVoiceFragment(0, source);
        } catch(...) {
            ERROR("Error thrown while handling packet!");
        }

        packet->Release();
    }

    void VoiceManager::HandleVoiceFragment(int decodedLength, GlobalNamespace::IConnectedPlayer* source) {
        if (!source) { // theoretically only in loopback situations
            _loopbackVoicePlayer->FeedFragment(_decodeSampleBuffer, decodedLength);
            return;
        }

        auto userId = source->get_userId();
        if (_chatManager->GetIsPlayerMuted(userId)) {
            _chatManager->SetPlayerIsSpeaking(userId, false);
            return;
        }

        auto voicePlayer = EnsurePlayerVoicePlayer(userId);
        voicePlayer->FeedFragment(_decodeSampleBuffer, decodedLength);
    }

    bool VoiceManager::get_canTransmit() const {
        return config.enableVoiceChat && _sessionManager->get_isConnected() && _sessionManager->get_isSyncTimeInitialized();
    }

    bool VoiceManager::StartVoiceTransmission() {
        if (!get_canTransmit()) return false;

        if (get_isTransmitting()) return true;

        if (!Audio::MicrophoneManager::get_hasMicrophonePermission())
            return false;

        _isTransmitting = true;
        _microphoneManager->StartCapture();

        _chatManager->SetLocalPlayerIsSpeaking(true);
        startedTransmittingEvent.invoke();
        return true;
    }

    bool VoiceManager::StopVoiceTransmission() {
        if (!get_isTransmitting()) return true;

        _microphoneManager->StopCapture();
        _isTransmitting = false;

        if (_sessionManager->get_isConnected()) {
            auto endPacket = Network::MpcVoicePacket::Obtain();

            endPacket->data = nullptr;
            _packetSerializer->Send(endPacket);

            endPacket->Release();
        }

        _chatManager->SetLocalPlayerIsSpeaking(false);
        stoppedTransmittingEvent.invoke();
        return true;
    }

    void VoiceManager::HandlePlayerMuted(StringW userId) {
        PlayerVoicePlayer* voicePlayer = nullptr;
        if (TryGetValue(_voicePlayers, userId, voicePlayer))
            voicePlayer->StopImmediate();
    }

    UnityEngine::AudioSource* VoiceManager::SetupLoopback() {
        if (!_loopbackTester || !_loopbackTester->m_CachedPtr.m_value) {
            auto go = UnityEngine::GameObject::New_ctor("VoiceLoopbackTester");
            _loopbackTester = go->AddComponent<UnityEngine::AudioSource*>();
        }

        _loopbackVoicePlayer->ConfigureAudioSource(_loopbackTester);

        _loopbackTester->get_gameObject()->SetActive(true);
        return _loopbackTester;
    }

    void VoiceManager::StartLoopbackTest() {
        StopLoopbackTest();

        SetupLoopback();

        _isLoopbackTesting = true;
        if (Audio::MicrophoneManager::get_hasMicrophonePermission())
            _microphoneManager->StartCapture();
    }

    void VoiceManager::StopLoopbackTest() {
        _microphoneManager->StopCapture();

        if (!get_isLoopbackTesting()) return;
        _isLoopbackTesting = false;
    }

    PlayerVoicePlayer* VoiceManager::EnsurePlayerVoicePlayer(StringW userId) {
        PlayerVoicePlayer* voicePlayer = nullptr;
        if (!TryGetValue(_voicePlayers, userId, voicePlayer)) {
            voicePlayer = PlayerVoicePlayer::New_ctor(userId, config.jitterBufferMs, config.spatialBlend);

            voicePlayer->startPlaybackEvent += {&VoiceManager::HandleVoicePlaybackStart, this};
            voicePlayer->stopPlaybackEvent += {&VoiceManager::HandleVoicePlaybackStop, this};

            _voicePlayers->Add(userId, voicePlayer);
        }

        return voicePlayer;
    }

    void VoiceManager::HandleVoicePlaybackStart(PlayerVoicePlayer* voicePlayer) {
        _chatManager->SetPlayerIsSpeaking(voicePlayer->get_playerUserId(), true);
    }

    void VoiceManager::HandleVoicePlaybackStop(PlayerVoicePlayer* voicePlayer) {
        _chatManager->SetPlayerIsSpeaking(voicePlayer->get_playerUserId(), false);
    }

    void VoiceManager::ProvideAvatarAudio(BeatSaber::AvatarCore::MultiplayerAvatarAudioController* avatarAudio) {
        auto player = avatarAudio->_connectedPlayer;

        auto voicePlayer = EnsurePlayerVoicePlayer(player->get_userId());
        voicePlayer->SetMultiplayerAvatarAudioController(avatarAudio);
    }
}
