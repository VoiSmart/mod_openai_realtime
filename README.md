# mod_openai_realtime 

![Build](https://github.com/VoiSmart/mod_openai_realtime/actions/workflows/build.yml/badge.svg?branch=main)
![Code-Checks](https://github.com/VoiSmart/mod_openai_realtime/actions/workflows/code-checks.yml/badge.svg?branch=main)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue?style=flat)](LICENSE)

**mod_openai_realtime** is a FreeSWITCH module that streams L16 audio from a channel to an OpenAI Realtime WebSocket endpoint. The stream follows OpenAI's Realtime API specification and enables real-time audio playback directly in the channel.  

> [!WARNING]
> This is a standalone fork of `mod_audio_stream`, not affiliated with the original project.
> Legacy naming (`mod_openai_audio_stream`) is retained for backward compatibility but will be updated in a future major release.

It is a fork of [mod_audio_stream](https://github.com/amigniter/mod_audio_stream), specifically adapted for streaming audio to OpenAI's Realtime API and playing the responses back to the user via FreeSWITCH and WebSocket.  

The goal of **mod_openai_realtime** is to provide a simple, lightweight, yet effective module for streaming audio and receiving responses directly from OpenAI’s Realtime WebSocket into the call through FreeSWITCH. It uses [ixwebsocket](https://machinezone.github.io/IXWebSocket/), a C++ WebSocket library compiled as a static library.  


## Important Notes 

* Use L16 format in your `session.update` to have the audio playback and temporal audio files creation to work properly. The module was tested with OpenAI's Realtime API set on L16 format. 
* You do not have to worry about the incoming sampling rate, the module resamples the audio to match the channels frame codec. 
* **Specify the OpenAI Realtime model in the URI**. For OpenAI Realtime PCM audio, the module now defaults the send rate to `24k`, so the basic command can be `uuid_openai_audio_stream ${uuid} start wss://api.openai.com/v1/realtime?model=gpt-realtime mono`. You can still override the send rate explicitly if needed.

## Installation

### Dependencies
To build the module, make sure `FreeSWITCH development headers`, `OpenSSL`, `Zlib` and `SpeexDSP` development packages are installed on your system.

Depending on your Linux distribution, you can install them like this:

#### Debian / Ubuntu

```bash
sudo apt-get install -y libfreeswitch-dev libssl-dev zlib1g-dev libspeexdsp-dev
```

#### RHEL / Fedora / Rocky

```bash
sudo dnf install -y freeswitch-devel openssl-devel zlib-devel speexdsp-devel
```

For other distributions, please refer to your package manager documentation to install the equivalent packages.

### Building
After cloning please execute: **git submodule init** and **git submodule update** to initialize the submodule.
#### Custom path
If you built FreeSWITCH from source, eq. install dir is /usr/local/freeswitch, add path to pkgconfig:
```
export PKG_CONFIG_PATH=/usr/local/freeswitch/lib/pkgconfig
```
To build the module, from the cloned repository:
```
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
make
sudo make install
```
**TLS** is `OFF` by default. To build with TLS support add `-DUSE_TLS=ON` to cmake line.

### Getting started

#### A simple dialplan example
The following is **a simple dialplan example** that demonstrates how to use the module to stream audio to OpenAI's Realtime API and play back the responses.

```xml
    <extension name="openai">
      <condition field="destination_number" expression="^.*$"> <!-- match all, change based on your needs -->
        <action application="set" data="STREAM_OPENAI_API_KEY=sk-xxxxxxxxxxxxxxxxxx" /> 
        <action application="set" data="STREAM_DISABLE_AUDIOFILES=true"/>
        <action application="answer" />
        <action application="set"
        data="api_result=${uuid_openai_audio_stream ${uuid} start wss://api.openai.com/v1/realtime?model=gpt-realtime mono}" />
        <action application="playback" data="silence_stream://-1//"/>
        <action application="set" data="api_result=${uuid_openai_audio_stream ${uuid} stop}"/> 
        <action application="hangup"/>
      </condition>
    </extension>
```

* Make sure to replace `sk-xxxxxxxxxxxxxxxxxx` with your actual OpenAI API key.
* The dialplan answers the call and starts streaming audio to OpenAI's Realtime API using `uuid_openai_audio_stream`, so you can try it out and see the OpenAI events in the FreeSWITCH console within the `mod_openai_audio_stream::json` events and other module events.
* The playback action with `silence_stream://-1//` is needed for audio playback to work properly. For more details, check issue [#16](https://github.com/VoiSmart/mod_openai_realtime/issues/16).

#### Next steps

The **getting started** example is a basic demonstration of how to use the module. For **more advanced usage**, we suggest piloting the module from an external application, for example using FreeSWITCH's Event Socket Library (ESL) or other methods to **receive events from FreeSWITCH** and send commands to **control the call flow**.

This way you can build more complex applications **allowing for function calls, updating instructions**, and other interactions with OpenAI's Realtime API. Check out the [OpenAI Realtime documentation](https://platform.openai.com/docs/guides/realtime) and [API reference](https://platform.openai.com/docs/api-reference/realtime) for more details on how to structure your requests and handle responses.

### Channel variables
The following channel variables can be used to fine-tune websocket connection and also configure mod_openai_realtime logging:

| Variable                               | Description                                             | Default |
| -------------------------------------- | ------------------------------------------------------- | ------- |
| STREAM_MESSAGE_DEFLATE                 | true or 1, disables per message deflate                 | off     |
| STREAM_HEART_BEAT                      | number of seconds, interval to send the heart beat      | off     |
| STREAM_SUPPRESS_LOG                    | true or 1, suppresses printing to log                   | off     |
| STREAM_BUFFER_SIZE                     | buffer duration in milliseconds, divisible by 20        | 20      |
| STREAM_EXTRA_HEADERS                   | JSON object for additional headers in string format     | none    |
| STREAM_NO_RECONNECT                    | true or 1, disables automatic websocket reconnection    | off     |
| STREAM_TLS_CA_FILE                     | CA cert or bundle, or the special values SYSTEM or NONE | SYSTEM  |
| STREAM_TLS_KEY_FILE                    | optional client key for WSS connections                 | none    |
| STREAM_TLS_CERT_FILE                   | optional client cert for WSS connections                | none    |
| STREAM_TLS_DISABLE_HOSTNAME_VALIDATION | true or 1 disable hostname check in WSS connections     | false   |
| STREAM_DISABLE_AUDIOFILES              | true or 1, disables debug audio files generation in tmp | false   |
| STREAM_OPENAI_API_KEY                  | OpenAI API key, used for authentication with OpenAI's   | none    |

- Per message deflate compression option is enabled by default. It can lead to a very nice bandwidth savings. To disable it set the channel var to `true|1`.
- Heart beat, sent every xx seconds when there is no traffic to make sure that load balancers do not kill an idle connection.
- Suppress parameter is omitted by default(false). All the responses from websocket server will be printed to the log. Not to flood the log you can suppress it by setting the value to `true|1`. Events are fired still, it only affects printing to the log.
- `Buffer Size` actually represents a duration of audio chunk sent to websocket. If you want to send e.g. 100ms audio packets to your ws endpoint
you would set this variable to 100. If ommited, default packet size of 20ms will be sent as grabbed from the audio channel (which is default FreeSWITCH frame size)
- Set `STREAM_OPENAI_API_KEY` to have a valid OpenAI API key to authenticate with OpenAI's Realtime API. This is required for the module to function properly. If not set the module will use the `STREAM_EXTRA_HEADERS` to pass the OpenAI API key as a header assuming you prepared the headers in the channel variable. **NOTE**: An OpenAI API key is required for the module to function properly. If not set, the module will not be able to connect to the API.

- Extra headers should be a JSON object with key-value pairs representing additional HTTP headers. Each key should be a header name, and its corresponding value should be a string.
  ```json
  {
      "Header1": "Value1",
      "Header2": "Value2",
      "Header3": "Value3"
  }
- Websocket automatic reconnection is on by default. To disable it set this channel variable to true or 1.
- TLS (for WSS) options can be fine tuned with the `STREAM_TLS_*` channel variables:
  - `STREAM_TLS_CA_FILE` the ca certificate (or certificate bundle) file. By default is `SYSTEM` which means use the system defaults.
Can be `NONE` which result in no peer verification.
  - `STREAM_TLS_CERT_FILE` optional client tls certificate file sent to the server.
  - `STREAM_TLS_KEY_FILE` optional client tls key file for the given certificate.
  - `STREAM_TLS_DISABLE_HOSTNAME_VALIDATION` if `true`, disables the check of the hostname against the peer server certificate.
Defaults to `false`, which enforces hostname match with the peer certificate.

## Raw Audio Mode

With raw audio mode enabled, the module acts as a bidirectional PCM16 audio bridge over WebSocket. This is intended for proxy servers and other backends that exchange raw PCM16 over WebSocket and want to avoid the JSON+base64 overhead used by the standard OpenAI path.

When `STREAM_RAW_AUDIO=true`, the module bypasses JSON+base64 encoding and decoding only for audio payloads and uses raw PCM16 binary WebSocket frames instead.

### How It Works

- **Send direction (User -> Server):** caller audio is sent as binary WebSocket frames containing raw PCM16 little-endian samples, instead of JSON `input_audio_buffer.append` messages with base64-encoded audio.
- **Receive direction (Server -> User):** binary WebSocket frames are treated as raw PCM16 audio and fed directly into the playback pipeline, with resampling applied if needed. Text WebSocket frames are still processed normally through the standard JSON message handler.

### Control Events

Because text frames continue to be processed through the normal `processMessage()` path even in raw audio mode, the proxy server can and should still send JSON text frames for control events.

| Feature | Required text event from proxy | Effect |
| --- | --- | --- |
| Barge-in (user interrupts playback) | `{"type":"input_audio_buffer.speech_started"}` | Clears audio queue and playback buffer |
| User speech stopped | `{"type":"input_audio_buffer.speech_stopped"}` | Logged; playback remains cleared until new audio arrives |
| Audio response complete | `{"type":"response.output_audio.done"}` | Sets response done flag and allows `openai_speech_stop` to fire after playback drains |
| Error reporting | Any JSON with `"type"` containing `"error"` | Logged as error |

Without these text events, the related features will not work correctly. In particular, without `response.output_audio.done`, the `mod_openai_audio_stream::openai_speech_stop` event will not fire after playback completes.

All other JSON text events, such as `session.updated` or `response.done`, continue to be forwarded as `mod_openai_audio_stream::json` events.

### Dialplan Example

```xml
<action application="answer" />
<action application="set" data="STREAM_RAW_AUDIO=true"/>
<action application="set" data="STREAM_DISABLE_AUDIOFILES=true"/>
<action application="set" data="api_result=${uuid_openai_audio_stream ${uuid} start ws://proxy-server:8080 mono 24k 16k}" />
<action application="playback" data="silence_stream://-1//"/>
```

### Proxy Requirements

A proxy server using raw audio mode must:

1. Accept binary WebSocket frames from the module containing raw PCM16 caller audio.
2. Send audio back as binary WebSocket frames containing raw PCM16.
3. Send control events as JSON text WebSocket frames.
4. Use PCM16 mono and a sample rate matching the configured `playback-rate`.

## API

### Commands
The freeswitch module exposes the following API commands:

```
uuid_openai_audio_stream <uuid> start <wss-url> <mix-type> [<send-rate>] [<playback-rate>] [mute_user]
```
Attaches a media bug and starts streaming audio (in L16 format) to the websocket server. Default send rate is 24k, matching the OpenAI Realtime API requirement. If send-rate differs from the channel codec rate, audio will be resampled. Passing `mute_user` delays forwarding caller audio to the Realtime API until you explicitly unmute.
- `uuid` - Freeswitch channel unique id
- `wss-url` - websocket url `ws://` or `wss://`
- `mix-type` - choice of
  - "mono" - single channel containing caller's audio
  - "mixed" - single channel containing both caller and callee audio
  - "stereo" - two channels with caller audio in one and callee audio in the other.
- `send-rate` - optional, the sample rate to which caller audio is resampled before sending to the server, choice of
  - "8k" = 8000 Hz
  - "16k" = 16000 Hz
  - "24k" = 24000 Hz (default)
  - or any multiple of 8000
- `playback-rate` - optional, the sample rate at which audio arrives from the server. The module resamples from this rate to the channel codec rate for playback. Choice of
  - "8k" = 8000 Hz
  - "16k" = 16000 Hz
  - "24k" = 24000 Hz (default)
  - or any multiple of 8000
  - If omitted, defaults to 24000 (OpenAI Realtime API rate). When using raw audio mode with a proxy that sends audio at a different rate, set this to match the proxy's output rate.
- `mute_user` - optional flag. When present, the module initialises muted and ignores caller audio until an explicit `unmute`.
- **IMPORTANT NOTE**: The OpenAI Realtime API, when using PCM audio format, expects the audio to be in 24 kHz sample rate. The module now defaults `send-rate` to `24k` for this reason, and mono remains the recommended mode for OpenAI Realtime. You can still override `send-rate` explicitly if you are targeting a different backend. From the OpenAI Realtime API documentation: *input audio must be 16-bit PCM at a 24kHz sample rate, single channel (mono), and little-endian byte order.* When using raw audio mode with a proxy server, the `playback-rate` parameter lets you specify the rate of audio the proxy sends back, avoiding pitch/speed distortion from incorrect resampling.
- **RAW AUDIO MODE NOTE**: See the [Raw Audio Mode](#raw-audio-mode) section below for the expected proxy contract, including required JSON control events such as `response.output_audio.done`.

```
uuid_openai_audio_stream <uuid> send_json
```
Sends a json object **base64 encoded** to the OpenAI websocket endpoint. Requires a valid `base64` text and a valid json compliant to the OpenAI Realtime API specification. The reason for base64 encoding is that spaces, new lines and other special characters in the json object can cause issues with the freeswitch API command parsing.

```
uuid_openai_audio_stream <uuid> stop 
```

```
uuid_openai_audio_stream <uuid> pause
```
Pauses audio streaming in both directions. Caller audio stops flowing to OpenAI and any OpenAI playback currently buffering into the channel is halted until `resume`.

```
uuid_openai_audio_stream <uuid> resume
```
Resumes audio streaming in both directions after a `pause`.

```
uuid_openai_audio_stream <uuid> mute [user | openai | all]
```
Keeps the media bug alive while silencing the selected leg. Defaults to `user` when omitted.
- `user`: block caller audio being sent to OpenAI.
- `openai`: block OpenAI playback from reaching the channel.
- `all`: apply both mute operations at once.

```
uuid_openai_audio_stream <uuid> unmute [user | openai | all]
```
Re-enables the selected audio leg after a corresponding `mute`. Defaults to `user` when omitted.

## Events
Module will generate the following event types:
- `mod_openai_audio_stream::json`
- `mod_openai_audio_stream::connect`
- `mod_openai_audio_stream::disconnect`
- `mod_openai_audio_stream::error`
- `mod_openai_audio_stream::play`

### response
Message received from websocket endpoint. Json expected, but it contains whatever the websocket server's response is.
#### Freeswitch event generated
**Name**: mod_openai_audio_stream::json
**Body**: WebSocket server response

### connect
Successfully connected to websocket server.
#### Freeswitch event generated
**Name**: mod_openai_audio_stream::connect
**Body**: JSON
```json
{
	"status": "connected"
}
```

### disconnect
Disconnected from websocket server.
#### Freeswitch event generated
**Name**: mod_openai_audio_stream::disconnect
**Body**: JSON
```json
{
	"status": "disconnected",
	"message": {
		"code": 1000,
		"reason": "Normal closure"
	}
}
```
- code: `<int>`
- reason: `<string>`

### error
There is an error with the connection. Multiple fields will be available on the event to describe the error.
#### Freeswitch event generated
**Name**: mod_openai_audio_stream::error
**Body**: JSON
```json
{
	"status": "error",
	"message": {
		"retries": 1,
		"error": "Expecting status 101 (Switching Protocol), got 403 status connecting to wss://localhost, HTTP Status line: HTTP/1.1 403 Forbidden\r\n",
		"wait_time": 100,
		"http_status": 403
	}
}
```
- retries: `<int>`, error: `<string>`, wait_time: `<int>`, http_status: `<int>`

### play
The audio playback is handled by the module.
OpenAI typically returns JSON objects containing base64 encoded audio to be played to the user. When `STREAM_RAW_AUDIO` is enabled with a compatible proxy, playback audio can also arrive as binary PCM frames.
The audio delta response may include other fields, but not so important for the audio playback.
In raw audio mode, binary PCM frames only carry audio data. Control and lifecycle expectations are described in the [Raw Audio Mode](#raw-audio-mode) section.
```json
{
  ...
  "type": "response.output_audio.delta",
  "delta": "BASE64_ENCODED_AUDIO...",
  ...
}
```

Event generated by the module (subclass: _mod_openai_audio_stream::play_) will be the same as the `data` element with the **file** added to it representing filePath:

This module will still generate audio files in the temp as `mod_audio_stream` does. Each file is written as a WAV container carrying the raw L16 audio received for playback, using the configured playback sample rate in the WAV header. Use L16 format in your `session.update` to have the audio playback and temporal audio files creation to work properly.
Can be useful for debugging purposes or other use cases. The `STREAM_DISABLE_AUDIOFILES` channel variable can be set to `true|1` to disable audio files events and generation.
```json
{
  "file": "/path/to/the/file"
}

```
If printing to the log is not suppressed, `response` printed to the console will look the same as the event. The original response containing base64 encoded audio is replaced because it can be quite huge.

the files generated by this feature will reside at the temp directory and will be deleted when the session is closed.
