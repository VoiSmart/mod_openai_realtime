# mod_openai_realtime

![Build](https://github.com/VoiSmart/mod_openai_realtime/actions/workflows/build.yml/badge.svg?branch=main)
![Tests](https://github.com/VoiSmart/mod_openai_realtime/actions/workflows/tests.yml/badge.svg?branch=main)
![Code-Checks](https://github.com/VoiSmart/mod_openai_realtime/actions/workflows/code-checks.yml/badge.svg?branch=main)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue?style=flat)](LICENSE)

**mod_openai_realtime** streams PCM16 audio bidirectionally between a FreeSWITCH channel and an OpenAI Realtime or compatible WebSocket endpoint.

> [!WARNING]
> This is a standalone fork of `mod_audio_stream`, not affiliated with the original project.
> Legacy naming (`mod_openai_audio_stream`) is retained for backward compatibility but will be updated in a future major release.

The module is based on [mod_audio_stream](https://github.com/amigniter/mod_audio_stream) and uses [IXWebSocket](https://machinezone.github.io/IXWebSocket/). Standard mode follows OpenAI Realtime JSON events; raw mode provides binary PCM16 transport for custom backends.

## Important Notes

- Configure official [OpenAI Realtime](https://developers.openai.com/api/reference/resources/realtime/client-events) sessions for PCM audio (`audio/pcm`, 24 kHz). The module transports little-endian PCM16 and resamples playback to the channel write codec.
- Specify the OpenAI Realtime model in the URI. Both stream directions default to `24k`, so the basic command is `uuid_openai_audio_stream ${uuid} start wss://api.openai.com/v1/realtime?model=gpt-realtime-2.1 mono`. Compatible backends using standard JSON events or raw audio may override either rate with a supported multiple of 8000 from 8000 through 48000.
- For raw PCM custom backends, prefer `uuid_raw_audio_stream ${uuid} start ...`. The older `STREAM_RAW_AUDIO=true` + `uuid_openai_audio_stream ... start ...` flow is deprecated, still supported for backward compatibility, and will be removed in the next major release.

## Installation

### Dependencies

To build the module on Linux with its default TLS support, install the FreeSWITCH development headers and the OpenSSL, Zlib, and SpeexDSP development packages. OpenSSL is optional only when configuring the build with `-DUSE_TLS=OFF`; other platforms use the TLS backend selected by IXWebSocket.

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

After cloning, initialize the IXWebSocket submodule:

```sh
git submodule update --init --recursive
```

#### Custom path

If FreeSWITCH was installed from source under `/usr/local/freeswitch`, add its pkg-config directory:

```sh
export PKG_CONFIG_PATH=/usr/local/freeswitch/lib/pkgconfig
```

To build the module, from the cloned repository:

```sh
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
make
sudo make install
```

TLS support is enabled by default. Pass `-DUSE_TLS=OFF` to CMake to build without TLS support; that build supports `ws://` endpoints but not `wss://` endpoints.

Normal module unload is refused while streams are attached; stop them before unloading.

### Tests

From the repository root, run the unit and integration suites with Docker:

```sh
TEST_ARTIFACT_DIR="$PWD/build/test-artifacts" ./tests/run-integration.sh
```

See [tests/README.md](tests/README.md) for prerequisites, the fast suite, coverage, and diagnostics.

### Getting started

#### A simple dialplan example

This dialplan streams audio to OpenAI's Realtime API and plays responses back into the call:

```xml
    <extension name="openai">
      <condition field="destination_number" expression="^.*$"> <!-- match all, change based on your needs -->
        <action application="set" data="STREAM_OPENAI_API_KEY=sk-xxxxxxxxxxxxxxxxxx" />
        <action application="set" data="STREAM_DISABLE_AUDIOFILES=true"/>
        <action application="answer" />
        <action application="set"
          data="api_result=${uuid_openai_audio_stream ${uuid} start wss://api.openai.com/v1/realtime?model=gpt-realtime-2.1 mono}" />
        <action application="playback" data="silence_stream://-1//"/>
        <action application="set" data="api_result=${uuid_openai_audio_stream ${uuid} stop}"/>
        <action application="hangup"/>
      </condition>
    </extension>
```

- Replace `sk-xxxxxxxxxxxxxxxxxx` with your OpenAI API key.
- The dialplan starts streaming with `uuid_openai_audio_stream`. Responses are exposed through `mod_openai_audio_stream::json` and the other module events.
- The `silence_stream://-1//` playback provides the media clock required for audio playback. See issue [#16](https://github.com/VoiSmart/mod_openai_realtime/issues/16).

#### Next steps

For production integrations, an external application can receive FreeSWITCH events and control the call through the Event Socket Library (ESL) or another FreeSWITCH interface.

This supports function calls, instruction updates, and other interactions with OpenAI's Realtime API. See the [OpenAI Realtime documentation](https://platform.openai.com/docs/guides/realtime) and [API reference](https://platform.openai.com/docs/api-reference/realtime) for request and response details.

### Channel variables

The following channel variables configure the WebSocket connection and module logging:

| Variable                               | Description                                             | Default |
| -------------------------------------- | ------------------------------------------------------- | ------- |
| STREAM_MESSAGE_DEFLATE                 | true or 1, disables per message deflate                 | off     |
| STREAM_HEART_BEAT                      | number of seconds (1 to 3600), interval to send the heart beat | off     |
| STREAM_SUPPRESS_LOG                    | true or 1, suppresses WebSocket payloads, URI details and final JSON in module logs; captured when the stream starts | off     |
| STREAM_BUFFER_SIZE                     | buffer duration in milliseconds, divisible by 20, max 1000 | 20      |
| STREAM_EXTRA_HEADERS                   | JSON object for additional headers in string format; merged with the Authorization header when STREAM_OPENAI_API_KEY is set (Authorization takes precedence) | none    |
| STREAM_NO_RECONNECT                    | true or 1, disables automatic WebSocket reconnection; after a close, an active stream drains queued playback before stopping, while a paused stream stops immediately | off     |
| STREAM_TLS_CA_FILE                     | CA cert or bundle, or the special values SYSTEM or NONE | SYSTEM  |
| STREAM_TLS_KEY_FILE                    | optional client key for WSS connections                 | none    |
| STREAM_TLS_CERT_FILE                   | optional client cert for WSS connections                | none    |
| STREAM_TLS_DISABLE_HOSTNAME_VALIDATION | true or 1 disable hostname check in WSS connections     | false   |
| STREAM_DISABLE_AUDIOFILES              | true or 1, disables debug audio files generation in tmp | false   |
| STREAM_OPENAI_API_KEY                  | OpenAI API key used for official OpenAI endpoints       | none    |
| STREAM_RAW_AUDIO                       | true or 1, deprecated legacy raw-mode switch for `uuid_openai_audio_stream` | false   |

- Per-message deflate is enabled by default; set `STREAM_MESSAGE_DEFLATE=true` to disable it.
- `STREAM_HEART_BEAT` keeps otherwise idle connections active through intermediaries such as load balancers.
- `STREAM_SUPPRESS_LOG=true` suppresses WebSocket payloads, URI details and final JSON in module logs without
  suppressing events. An active stream keeps the value captured when it started; changing the channel variable takes
  effect on the next stream.
- `STREAM_BUFFER_SIZE` is the duration of each caller-audio chunk sent to the backend. It defaults to the 20 ms FreeSWITCH frame duration.
- Authenticate official OpenAI endpoints with `STREAM_OPENAI_API_KEY` or an `Authorization` header in
  `STREAM_EXTRA_HEADERS`. Compatible backends may use their own headers or require no authentication.
- `start` rejects CR/LF characters in the WebSocket URI, extra header names or values, and API key.

- Extra headers should be a JSON object with key-value pairs representing additional HTTP headers. Each key should be a header name, and its corresponding value should be a string.
  ```json
  {
      "Header1": "Value1",
      "Header2": "Value2",
      "Header3": "Value3"
  }
  ```
- WebSocket automatic reconnection is enabled by default. Set `STREAM_NO_RECONNECT=true` to disable it.
- TLS (for WSS) options can be fine-tuned with the `STREAM_TLS_*` channel variables:
  - `STREAM_TLS_CA_FILE` selects a CA certificate or bundle. `SYSTEM` uses the system defaults; `NONE` disables peer verification.
  - `STREAM_TLS_CERT_FILE` selects an optional client TLS certificate.
  - `STREAM_TLS_KEY_FILE` selects the corresponding optional client key.
  - `STREAM_TLS_DISABLE_HOSTNAME_VALIDATION` if `true`, disables the check of the hostname against the peer server certificate.
    It defaults to `false`, which enforces a hostname match.

### Runtime Limits

The module bounds JSON nesting, application-level peer processing and playback memory:

- After IXWebSocket has assembled an inbound text or binary message, payloads larger than 8 MiB are dropped before JSON, Base64, or PCM processing. This does not bound WebSocket transport aggregation or decompression.
- JSON nested deeper than 128 levels is rejected before parsing: backend messages are dropped, while `send_json` and final `stop` payloads return `-ERR` (stop still completes).
- Decoded playback audio is queued up to 180 seconds. Audio that would exceed this capacity is dropped; overflow logging is re-enabled after the backlog falls below half capacity.
- Debug WAV files accumulate until stream cleanup, with no per-stream disk limit. Set `STREAM_DISABLE_AUDIOFILES=true` to disable them.

## Raw Audio Mode

With raw audio mode enabled, the module acts as a bidirectional PCM16 audio bridge over WebSocket. This is intended for compliant custom backends that exchange raw PCM16 over WebSocket and want to avoid the JSON+base64 overhead used by the standard OpenAI path.

Raw audio mode can be enabled in two ways:

- Preferred: start the stream with `uuid_raw_audio_stream`.
- Deprecated legacy path: set `STREAM_RAW_AUDIO=true` and start with `uuid_openai_audio_stream`. This remains supported for backward compatibility, emits a runtime warning, and will be removed in the next major release.

In both cases the module bypasses JSON+base64 encoding and decoding only for audio payloads and uses raw PCM16 binary WebSocket frames instead.

### How It Works

- **Send direction (User -> Server):** caller audio is sent as binary WebSocket frames containing raw PCM16 little-endian samples, instead of JSON `input_audio_buffer.append` messages with base64-encoded audio.
- **Receive direction (Server -> User):** binary WebSocket frames are treated as raw PCM16 audio and fed directly into the playback pipeline, with resampling applied if needed. Text WebSocket frames are still processed normally through the standard JSON message handler.

### Control Events

Because text frames continue to be processed through the normal `processMessage()` path even in raw audio mode, the backend can and should still send JSON text frames for control events.

| Feature | Required text event from backend | Effect |
| --- | --- | --- |
| Barge-in (user interrupts playback) | `{"type":"input_audio_buffer.speech_started"}` | Clears audio queue and playback buffer; fires `openai_speech_stop` if playback was active |
| User speech stopped | `{"type":"input_audio_buffer.speech_stopped"}` | Logged; playback remains cleared until new audio arrives |
| Audio response complete | `{"type":"response.output_audio.done"}` | Sets response done flag and allows `openai_speech_stop` to fire after playback drains |
| Error reporting | Any JSON with `"type"` containing `"error"` | Logged as error |

Without these text events, the related features will not work correctly. In particular, without `response.output_audio.done`, the `mod_openai_audio_stream::openai_speech_stop` event will not fire after playback completes.

All JSON text events, including the control events above, continue to be forwarded as
`mod_openai_audio_stream::json` events unless they contain a valid audio delta consumed for playback.

### Dialplan Example

```xml
<action application="answer" />
<action application="set" data="STREAM_DISABLE_AUDIOFILES=true"/>
<action application="set" data="api_result=${uuid_raw_audio_stream ${uuid} start ws://backend:8080 mono 24k 16k}" />
<action application="playback" data="silence_stream://-1//"/>
```

If you still need the deprecated legacy path for compatibility, this remains valid for now:

```xml
<action application="set" data="STREAM_RAW_AUDIO=true"/>
<action application="set" data="api_result=${uuid_openai_audio_stream ${uuid} start ws://backend:8080 mono 24k 16k}" />
```

### Backend Requirements

A compliant custom backend using raw audio mode must:

1. Accept little-endian PCM16 capture frames at the configured `send-rate`. The capture is mono for `mono` and
   `mixed`, or two-channel interleaved PCM16 for `stereo`.
2. Send little-endian mono PCM16 playback frames at the configured `playback-rate`.
3. Send control events as JSON text WebSocket frames.

## API

### Commands

The FreeSWITCH module exposes the following API commands:

```text
uuid_openai_audio_stream <uuid> start <ws-uri> <mix-type> [<send-rate>] [<playback-rate>] [mute_user]
```
Attaches a media bug and starts streaming PCM16 audio to the WebSocket server. The default send rate is 24 kHz, matching the OpenAI Realtime API requirement. If `send-rate` differs from the channel codec rate, audio is resampled. Passing `mute_user` delays caller audio until an explicit `unmute`.

- `uuid` - FreeSWITCH channel unique ID
- `ws-uri` - WebSocket URL using either `ws://` or `wss://`. A query without a path is preserved by adding `/`:
  `ws://host?x=1` becomes `ws://host/?x=1`.
- `mix-type` - choice of
  - "mono" - single channel containing caller's audio
  - "mixed" - single channel containing both caller and callee audio
  - "stereo" - two channels with caller audio in one and callee audio in the other.
- `send-rate` - optional, the sample rate to which caller audio is resampled before sending to the server, choice of
  - "8k" = 8000 Hz
  - "16k" = 16000 Hz
  - "24k" = 24000 Hz (default)
  - or a decimal multiple of 8000 up to 48000 (`32000`, `40000`, or `48000`)
- `playback-rate` - optional, the sample rate at which audio arrives from the server. The module resamples from this rate to the channel codec rate for playback. Choice of
  - "8k" = 8000 Hz
  - "16k" = 16000 Hz
  - "24k" = 24000 Hz (default)
  - or a decimal multiple of 8000 up to 48000 (`32000`, `40000`, or `48000`)
  - If omitted, defaults to 24000 (OpenAI Realtime API rate). For a compatible backend using standard JSON events or raw audio at another rate, set this to match the source audio.
- `mute_user` - optional flag. When present, the module initialises muted and ignores caller audio until an explicit `unmute`.
- Official OpenAI Realtime PCM uses `audio/pcm` at a fixed 24 kHz rate, so the module defaults both rates to `24k`; mono is recommended. Compatible backends using standard JSON events or raw audio can override either rate. Set `playback-rate` to the rate sent by the backend to avoid pitch or speed distortion from incorrect resampling.
- See [Raw Audio Mode](#raw-audio-mode) for the backend contract, including required JSON control events such as `response.output_audio.done`.

```text
uuid_raw_audio_stream <uuid> start <ws-uri> <mix-type> [<send-rate>] [<playback-rate>] [mute_user]
```
Uses the same arguments as `uuid_openai_audio_stream ... start ...`, but forces raw PCM16 WebSocket audio framing without requiring the deprecated `STREAM_RAW_AUDIO=true` channel variable. This is the preferred entry point for compliant custom raw-audio backends.

All lifecycle commands (`stop`, `pause`, `resume`, `mute`, `unmute`, and `send_json`) are available on both `uuid_openai_audio_stream` and `uuid_raw_audio_stream`, because `uuid_raw_audio_stream` only changes how `start` selects raw audio mode and does not create a separate control plane. For clarity and consistency, prefer controlling the stream through the same API family used for `start`.

```text
uuid_openai_audio_stream <uuid> send_json <base64json>
```
Sends one complete, NUL-free UTF-8 JSON value accepted by cJSON to the WebSocket endpoint, without additional
strict JSON syntax checks. The command requires structurally valid Base64, which protects spaces, newlines, and
other characters from FreeSWITCH API parsing. The decoded bytes are forwarded unchanged rather than reserialized.

```text
uuid_openai_audio_stream <uuid> stop [<base64json>]
```
Stops the stream. The optional payload follows the same validation rules as `send_json` and is sent before the
WebSocket closes. An invalid final payload is not sent and makes the command return `-ERR`, but teardown still
completes.

```text
uuid_openai_audio_stream <uuid> pause
```
Pauses audio streaming in both directions. Caller audio stops flowing to OpenAI and any OpenAI playback currently buffering into the channel is halted until `resume`.

```text
uuid_openai_audio_stream <uuid> resume
```
Resumes audio streaming in both directions after a `pause`.

```text
uuid_openai_audio_stream <uuid> mute [user | openai | all]
```
Keeps the media bug alive while silencing the selected leg. Defaults to `user` when omitted.

- `user`: block caller audio being sent to OpenAI.
- `openai`: block OpenAI playback from reaching the channel.
- `all` (alias `both`): attempt both mute operations; an error does not undo either operation.

When `mute` changes caller audio from unmuted to muted, the module flushes buffered caller audio and sends
a block containing one second of silence if the WebSocket is connected.

```text
uuid_openai_audio_stream <uuid> unmute [user | openai | all]
```
Re-enables the selected audio leg after a corresponding `mute`. Defaults to `user` when omitted.

### Command responses

Successful commands retain the response `+OK Success`. Failures return one `-ERR` line with a reason:

```text
-ERR Stream not found
-ERR Stream already exists
-ERR Invalid send sample rate; expected a multiple of 8000 from 8000 to 48000
```

Check the `+OK` or `-ERR` prefix in clients; diagnostic wording is intended for humans. Error replies do not echo
connection URIs, credentials or JSON payloads. Calling either API without arguments still displays its usage.

For `start`, `+OK` confirms local startup; wait for `connect` before treating the WebSocket as connected. Connection
failures are asynchronous `error` events. Some control failures occur after a state change: a rejected final stop
payload returns `Stream stopped; final message failed: ...`, while failure to send mute silence reports
`User audio muted; ...`. In both cases the indicated stop or mute has already taken effect.

## Events

The module generates the following event types:

- `mod_openai_audio_stream::json`
- `mod_openai_audio_stream::connect`
- `mod_openai_audio_stream::disconnect`
- `mod_openai_audio_stream::error`
- `mod_openai_audio_stream::play`
- `mod_openai_audio_stream::openai_speech_start`
- `mod_openai_audio_stream::openai_speech_stop`

In raw audio mode, control messages from the backend, such as `input_audio_buffer.speech_started` and `input_audio_buffer.speech_stopped`, are still received as JSON text frames and handled through the normal message-processing path. They are not emitted as dedicated FreeSWITCH events by the module. Instead:

- `input_audio_buffer.speech_started` is used internally for barge-in, clearing queued playback audio, and is also
  forwarded through the normal JSON event flow. This typically corresponds to VAD being triggered by the backend.
- `input_audio_buffer.speech_stopped` is logged and forwarded through the normal JSON event flow.
- `mod_openai_audio_stream::openai_speech_start` is emitted when playback starts, with body `{"status":"started"}`.
- `mod_openai_audio_stream::openai_speech_stop` is emitted when playback drains after `response.output_audio.done`, or is interrupted by barge-in, with body `{"status":"stopped"}`.

### json

Forwards a text message received from the WebSocket endpoint.

#### FreeSWITCH event generated

**Name**: mod_openai_audio_stream::json
**Body**: WebSocket server response

### connect

Successfully connected to the WebSocket server.

#### FreeSWITCH event generated

**Name**: mod_openai_audio_stream::connect
**Body**: JSON
```json
{
	"status": "connected"
}
```

### disconnect

Disconnected from the WebSocket server.

#### FreeSWITCH event generated

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

A WebSocket connection attempt failed. The event contains the transport diagnostics.

#### FreeSWITCH event generated

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
- retries: `<int>`, error: `<string>`, wait_time: `<number, milliseconds>`, http_status: `<int>`

Connection failures log these diagnostics by default. With `STREAM_SUPPRESS_LOG=true`, details remain available in
the event body but are omitted from the module log because an error reason can contain the connection URI.

### play

OpenAI typically returns JSON objects containing Base64-encoded audio to be played to the user. When raw audio mode is enabled with a compatible custom backend, playback audio can also arrive as binary PCM frames.
Audio delta events may include additional fields; playback requires only `type` and `delta`.
The `delta` must be structurally valid standard or URL-safe Base64; padding is optional. Malformed audio is rejected
instead of being decoded partially.
In raw audio mode, binary PCM frames only carry audio data. Control and lifecycle expectations are described in the [Raw Audio Mode](#raw-audio-mode) section.
```json
{
  ...
  "type": "response.output_audio.delta",
  "delta": "BASE64_ENCODED_AUDIO...",
  ...
}
```

By default, the module writes received PCM16 playback chunks as temporary WAV files using the configured playback
sample rate. For a JSON audio delta, `mod_openai_audio_stream::play` preserves the other fields, removes the Base64
`delta`, and adds `file`. A raw binary chunk produces an event containing only `file`. PCM16 samples split across
consecutive JSON deltas or raw binary frames are joined before a debug file is emitted, so an input message does not
necessarily map one-to-one to a WAV.
The `play` event reports file creation, not whether the audio reached the caller.

```json
{
  "type": "response.output_audio.delta",
  "file": "/path/to/the/file"
}
```

Set `STREAM_DISABLE_AUDIOFILES=true` to disable both file generation and `play` events. The module also removes the Base64 audio from its debug log. Temporary files are removed when the stream is cleaned up.
