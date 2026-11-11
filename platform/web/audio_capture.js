(function(root) {
  'use strict';

  function inputError(error) {
    switch (error.name) {
      case 'NotAllowedError':
      case 'SecurityError':
        return 'permission';
      case 'NotFoundError':
      case 'OverconstrainedError':
      case 'NotSupportedError':
        return 'unavailable';
      case 'AbortError':
        return 'interrupted';
      default:
        return 'failed';
    }
  }

  function start(id) {
    let input = null;
    let reader = null;
    let timer = 0;
    let pcm = new Int16Array(0);
    let plane = new Float32Array(0);

    function stop() {
      const retired = id;
      id = 0;
      clearTimeout(timer);
      if (input) {
        for (const track of input.getTracks()) track.stop();
        input = null;
      }
      if (reader) {
        void reader.cancel().catch(() => {});
        reader = null;
      }
      return retired;
    }

    function finish(kind, message) {
      const retired = stop();
      if (retired) Module.huxeruiAudioCaptureFinish(retired, kind, message);
    }

    async function capture() {
      if (!root.isSecureContext || typeof root.navigator?.mediaDevices?.getUserMedia !== 'function' ||
          typeof root.MediaStreamTrackProcessor !== 'function' || typeof root.AudioData !== 'function') {
        finish('unavailable', 'Microphone capture requires a secure context and a browser with audio track processing.');
        return;
      }
      try {
        const stream = await root.navigator.mediaDevices.getUserMedia({
          audio: {
            channelCount: {ideal: 1},
            sampleRate: {ideal: 48000},
            echoCancellation: false,
            noiseSuppression: false,
            autoGainControl: false,
          },
        });
        if (!id) {
          for (const track of stream.getTracks()) track.stop();
          return;
        }
        input = stream;
        const track = stream.getAudioTracks()[0];
        if (!track) {
          finish('unavailable', 'The browser did not open a microphone track.');
          return;
        }
        try {
          const processor = new root.MediaStreamTrackProcessor({track, maxBufferSize: 16});
          reader = processor.readable.getReader();
        } catch (_) {
          finish('unavailable', 'This browser cannot read timestamped PCM from a microphone track.');
          return;
        }
        track.addEventListener('ended', () => finish('interrupted', 'The microphone track ended.'), {once: true});
        timer = setTimeout(() => finish('failed', 'The microphone produced no timestamped PCM.'), 3000);
        while (id) {
          const {done, value: frame} = await reader.read();
          if (!id || done) {
            if (frame) frame.close();
            break;
          }
          try {
            const channels = frame.numberOfChannels;
            const frames = frame.numberOfFrames;
            const count = frames * channels;
            if ((channels !== 1 && channels !== 2) || !frames || count > 256 * 1024 ||
                !Number.isSafeInteger(frame.timestamp) || !Number.isInteger(frame.sampleRate) ||
                frame.sampleRate <= 0 || frame.sampleRate > 0xffffffff) {
              throw new Error('The browser returned an unsupported microphone frame.');
            }
            if (pcm.length !== count) pcm = new Int16Array(count);
            if (plane.length !== frames) plane = new Float32Array(frames);
            for (let channel = 0; channel < channels; ++channel) {
              frame.copyTo(plane, {planeIndex: channel, format: 'f32-planar'});
              for (let frameIndex = 0; frameIndex < frames; ++frameIndex) {
                const value = plane[frameIndex];
                pcm[frameIndex * channels + channel] = Number.isFinite(value)
                  ? Math.min(32767, Math.max(-32768, value * 32768))
                  : 0;
              }
            }
            if (!Module.huxeruiAudioCapturePublish(id, frame.sampleRate, channels, frame.timestamp, pcm)) {
              finish('', '');
              return;
            }
            clearTimeout(timer);
          } finally {
            frame.close();
          }
        }
        if (id) finish('interrupted', 'The microphone track ended.');
      } catch (error) {
        if (id) finish(inputError(error), String(error.message || error));
      }
    }

    // Let the C++ caller retain the stop handle before reporting any startup error.
    queueMicrotask(() => { void capture(); });
    return {stop};
  }

  root.HuxerAudioCapture = {start};
})(globalThis);
