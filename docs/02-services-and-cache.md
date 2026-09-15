# Services and Cache

[Back to README](../README.md)

## Online Services

The application loads popular stations from [Radio Browser](https://www.radio-browser.info/)
at startup. Search, refresh, and pagination use the same endpoint with different
query parameters. Servers are tried in this order:

| Priority | Endpoint |
| --- | --- |
| First choice | `https://all.api.radio-browser.info/json/stations/search` |
| First fallback | `https://de1.api.radio-browser.info/json/stations/search` |
| Final fallback | `https://de2.api.radio-browser.info/json/stations/search` |

Requests sort by click count in descending order and set `hidebroken=true`.
Name searches trim surrounding whitespace and send a percent-encoded `name`
parameter. A page covers 50 records; the client requests one extra record to
determine whether **Load more** is available. Invalid records and duplicate
UUIDs or stream URLs are filtered, so a page can add fewer than 50 stations.
The client prefers `url_resolved` and falls back to `url` when needed.

Starting a new query cancels the previous request. Successful searches replace
the displayed results, including replacing them with an empty list when there
are no matches. Pagination appends results. Neither action changes the selected
station snapshot or interrupts playback.

### Request Limits and Errors

Radio Browser requests invoke `curl` directly with argument arrays. They disable
the default curl configuration file, use the User-Agent `lilygo-ui-radio/1.0`,
and allow only HTTPS for both requests and redirects.

| Limit | Value |
| --- | --- |
| Connection timeout | 5 seconds per server |
| Total curl timeout | 12 seconds per server |
| Response body | 2 MiB |
| Search text | 1,024 bytes |

A failed transfer, oversized response, or invalid JSON advances to the next
server. Missing `curl` and local process or pipe errors are reported directly.
Failures retain the existing list and show a directory error. Before the first
successful response, that list contains four bundled SomaFM streams: Groove
Salad, Drone Zone, Deep Space One, and Secret Agent. These are network stream
addresses, not downloaded audio.

Station names entered in search are sent to Radio Browser. Artwork requests go
to the station's `favicon` host, and playback connects to the broadcaster's
stream host. Artwork and streams may use HTTP as well as HTTPS; the directory's
HTTPS requirement does not apply to those resources.

## Playback

Audio is decoded by `ffmpeg` to 48 kHz, signed 16-bit little-endian stereo PCM.
The output process depends on the build platform:

| Platform | Pipeline | Output device |
| --- | --- | --- |
| Linux simulator and CM0 | `ffmpeg -> aplay` | ALSA `default` |
| macOS simulator | `ffmpeg -> ffplay` | System default through SDL |

Install `ffmpeg`, `curl`, and `alsa-utils` on Linux. macOS requires `ffmpeg`,
`ffplay`, and `curl`; `brew install ffmpeg` provides both audio executables.
The required programs must be visible in the application's launch-time `PATH`.
See the [README](../README.md) for build prerequisites and commands.

Custom stream URLs must use `http://` or `https://`, include a host, and fit
within 1,023 bytes after surrounding whitespace is trimmed. Internal whitespace
and control characters are rejected. MP3, AAC, OGG, and other formats depend on
the installed ffmpeg build and the remote stream. URL validation does not
establish that a server is reachable or that its response contains playable
audio.

The decoder enables reconnects for streamed input, a maximum reconnect delay of
5 seconds, and a 15-second read/write timeout. The interface shows **Connecting...**
after spawning the player, then **Playing** after the process has remained alive
for 800 ms. This state is based on process lifetime, not confirmation that audio
has reached the speaker.

Pause and resume suspend and continue the entire playback process group.
Stopping, switching streams, and application shutdown terminate the old process
group. A missing executable or failed player process appears as a playback
error, independently of directory loading and artwork.

### Player Log

The decoder and audio sink append their diagnostic output to:

```text
/tmp/lilygo-ui-radio.log
```

Inspect it when a stream fails or no sound is audible:

```sh
tail -n 80 /tmp/lilygo-ui-radio.log
```

The path is fixed and the application does not rotate this file. Directory and
artwork subprocess diagnostics are discarded; their errors appear as directory
status or an artwork placeholder instead of entries in the player log.

## Artwork Cache

Artwork comes from the station's Radio Browser `favicon` field. It is usually a
station logo, not the current song or programme cover. Missing, pending, or
failed artwork uses the default radio icon.

A background worker downloads with `curl` and decodes with `ffmpeg`. Supported
source formats are PNG, JPEG, ICO, WebP, BMP, and GIF, detected from their bytes
rather than their URL extension. Only the first frame is displayed. The source
is scaled while preserving its aspect ratio and padded with transparency to
produce a `256 x 256` BGRA image in memory.

| Limit | Value |
| --- | --- |
| Artwork URL | HTTP/HTTPS, at most 4,096 bytes |
| Download size | 2 MiB |
| Redirects | At most 5, restricted to HTTP/HTTPS |
| Connection / transfer timeout | 4 / 12 seconds |
| Download worker deadline | 14 seconds |
| Decode deadline | 8 seconds |
| Original image size | At most 4,194,304 pixels |
| Memory entries | At most 128 pending, ready, or failed entries |
| Failed-entry retry delay | 5 minutes while the entry remains cached |
| Disk entries | At most 100 original images after cache pruning |
| Disk validity | 7 days from the metadata file's modification time |

The decoder is restricted to the supported raster-image formats and local
file/pipe input. SVG and other unsupported content use the placeholder.

### Cache Paths and Validation

The cache directory is selected in this order:

| Condition | Cache directory |
| --- | --- |
| `LILYGO_UI_RADIO_ARTWORK_CACHE` is set | The exact value; an empty string disables disk caching |
| Nonempty `XDG_CACHE_HOME` | `$XDG_CACHE_HOME/lilygo/ui/radio-artwork` |
| Nonempty `HOME` | `$HOME/.cache/lilygo/ui/radio-artwork` |
| No usable path | Disk caching is disabled |

Files use a hash of the source URL, followed by the detected extension, with a
matching `.meta` sidecar. The sidecar records the cache version, complete source
URL, format, byte length, and content checksum. Original downloaded bytes are
stored unchanged; images are not resized or transcoded on disk.

A valid local source is decoded without downloading it again. Missing, expired,
damaged, or undecodable entries trigger a fresh download. New entries are
written through temporary files and renamed into place. After a successful
write, pruning removes the oldest metadata entries over the 100-image limit
and removes recognized legacy `.radio-art` files.

The view releases decoded images once they are no longer needed. Revisiting a
station can therefore decode its original file again. Cache write failures do
not prevent successfully decoded artwork from displaying in the current
session. Disabling disk caching still permits downloads and in-memory images.
There is no cache-clearing control in the interface; the internal cache
`clear()` operation cancels work and clears memory while retaining disk files.

## Configuration and Favorites

The configuration file stores the last accepted custom stream URL. Its path is
selected in this order:

| Condition | Configuration file |
| --- | --- |
| Nonempty `LILYGO_UI_RADIO_CONFIG` | The exact configured file path |
| Nonempty `XDG_CONFIG_HOME` | `$XDG_CONFIG_HOME/lilygo/ui/radio.conf` |
| Nonempty `HOME` | `$HOME/.config/lilygo/ui/radio.conf` |
| No usable path | Settings and favorites remain in memory for the session |

An empty `LILYGO_UI_RADIO_CONFIG` falls through to the default paths; it does
not disable persistence. This differs from the artwork cache override.

The configuration format is a single setting:

```ini
custom_url=https://ice3.somafm.com/groovesalad-128-mp3
```

The URL is saved after validation, before playback starts, so a failed playback
attempt can still update the saved value. Saving uses a temporary file and
rename, but is best effort: write failures do not block playback and are not
shown in the interface. Missing or invalid stored URLs are ignored. Loading a
saved URL prefills the custom-stream dialog; it does not select or play the
stream automatically.

Favorites are stored at the configuration path with `.favorites` appended,
normally `radio.conf.favorites`. Up to 256 complete station snapshots are
saved, including their name, stream URL, UUID, country, codec, bitrate, tags,
country code, language, and favicon URL. Stations match by a nonempty UUID or
stream URL. A favorite remains playable without a fresh catalog lookup.

The favorites file uses the versioned `LILYGO_RADIO_FAVORITES` format and is
limited to 4 MiB. Loading rejects a damaged or unsupported file as a whole and
reports an error. Writes use a private temporary file, `fsync`, and rename. A
failed write retains the previous collection and shows a storage error.

### Configuration Examples

Keep simulator data in explicit locations; parent directories are created when
data is saved:

```sh
LILYGO_UI_RADIO_CONFIG="$HOME/radio-data/radio.conf" \
LILYGO_UI_RADIO_ARTWORK_CACHE="$HOME/radio-data/artwork" \
  ./build/host-simulator/lilygo-ui-radio
```

Run with artwork disk caching disabled:

```sh
LILYGO_UI_RADIO_ARTWORK_CACHE='' \
  ./build/host-simulator/lilygo-ui-radio
```

## Sleep Timer

The timer offers 15, 30, 45, 60, and 90 minutes. It uses a monotonic clock and
stops playback when its deadline is reached. Choosing another duration starts
a new countdown from that moment. Pausing, resuming, and switching stations
leave the existing deadline unchanged.

Canceling the timer leaves playback running. Manually stopping playback also
cancels the timer. The timer exists only for the current application session;
its state and the playback selection are not persisted.

## Offline Operation

This project has no application-level network-disable environment variable or
offline-mode switch. Startup and catalog controls attempt Radio Browser
requests. Audio is streamed live and is not cached for offline playback.

When the directory is unavailable, the existing station list, saved favorites,
and custom URL remain usable. Streams still require access to their broadcast
hosts and a working audio device. Valid cached logos can display without
network access, provided `ffmpeg` is available to decode them; missing or
expired artwork attempts a download and falls back to the default icon if it
fails.
