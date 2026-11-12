#!/bin/sh
# mux_mp4.sh - wrap a Venus elementary stream (.h264/.h265) into MP4 with the
# host ffmpeg using stream copy (no re-encode), then verify the result.
#
# Usage:
#   tools/mux_mp4.sh [options] <input.h264|input.h265> <output.mp4>
#
# Options:
#   -c, --codec h264|hevc   elementary stream codec (default: from extension)
#   -f, --fps N             frame rate hint for the raw ES demuxer (default 30;
#                           raw HEVC ES carries no VUI timing, so this matters)
#   -q, --quiet             only print errors
#   -n, --no-verify         skip the ffprobe / software-decode verification
#   -h, --help
#
# The verification runs:
#   1. `ffmpeg -v error -i <es> -f null -`  (software decode of the bitstream)
#   2. `ffprobe` on the MP4 (codec, size, frame rate, frame count)
#
# Note: the Venus firmware writes H.264 level_idc 5.1 even for 4208x3120
# (which is above level 5.1 MaxFS); decoders accept it.  See
# docs/measurements-encode.md for details and workarounds.

set -eu

FPS=30
CODEC=""
VERIFY=1
QUIET=0

usage() {
    sed -n '2,25p' "$0" | sed 's/^# \{0,1\}//'
    exit 0
}

while [ $# -gt 0 ]; do
    case "$1" in
    -c|--codec)   CODEC="${2:?-c needs a value}"; shift 2 ;;
    -f|--fps)     FPS="${2:?-f needs a value}"; shift 2 ;;
    -n|--no-verify) VERIFY=0; shift ;;
    -q|--quiet)   QUIET=1; shift ;;
    -h|--help)    usage ;;
    --)           shift; break ;;
    -*)           echo "mux_mp4.sh: unknown option '$1'" >&2; exit 2 ;;
    *)            break ;;
    esac
done

if [ $# -lt 2 ]; then
    echo "usage: $0 [-c h264|hevc] [-f fps] <in.h264|in.h265> <out.mp4>" >&2
    exit 2
fi

ES=$1
MP4=$2

if [ ! -f "$ES" ]; then
    echo "mux_mp4.sh: no such file: $ES" >&2
    exit 1
fi
if [ -s "$ES" ]; then :; else
    echo "mux_mp4.sh: elementary stream is empty: $ES" >&2
    exit 1
fi

if [ -z "$CODEC" ]; then
    case "$ES" in
    *.h264|*.264|*.h26l) CODEC=h264 ;;
    *.h265|*.265|*.hevc) CODEC=hevc ;;
    *)
        echo "mux_mp4.sh: cannot infer codec from '$ES'; use -c h264|hevc" >&2
        exit 2
        ;;
    esac
fi
case "$CODEC" in
h264|hevc) : ;;
*) echo "mux_mp4.sh: bad codec '$CODEC'" >&2; exit 2 ;;
esac

log() { [ "$QUIET" -eq 1 ] || echo "mux_mp4: $*"; }

# Raw H.264/HEVC ES packets carry no timestamps; without setts the mov muxer
# invents a broken (roughly 60fps) timeline.  Stamp every packet on a 1/fps
# time base so the MP4 is an exact CFR stream.
SETTS="setts=time_base=1/${FPS}:pts=N:dts=N:duration=1"

log "stream copy $ES ($CODEC, ${FPS} fps) -> $MP4"
ffmpeg -nostdin -hide_banner -loglevel error -y \
    -framerate "$FPS" -f "$CODEC" -i "$ES" \
    -c copy -bsf:v "$SETTS" -movflags +faststart "$MP4"

if [ ! -s "$MP4" ]; then
    echo "mux_mp4.sh: ffmpeg produced no output" >&2
    exit 1
fi

if [ "$VERIFY" -eq 1 ]; then
    log "verifying elementary stream decode (software):"
    if ! ffmpeg -nostdin -v error -i "$ES" -f null - 2>/tmp/mux_mp4_decode.$$; then
        cat /tmp/mux_mp4_decode.$$ >&2
        rm -f /tmp/mux_mp4_decode.$$
        echo "mux_mp4.sh: bitstream failed to decode" >&2
        exit 1
    fi
    rm -f /tmp/mux_mp4_decode.$$
    log "decode: OK"

    log "ffprobe $MP4:"
    ffprobe -v error -select_streams v:0 -count_frames \
        -show_entries stream=codec_name,profile,level,width,height,r_frame_rate,avg_frame_rate,nb_read_frames \
        -of default=noprint_wrappers=1 "$MP4" | sed 's/^/  /'
    [ "$QUIET" -eq 1 ] || echo "mux_mp4: OK"
else
    [ "$QUIET" -eq 1 ] || echo "mux_mp4: OK (no verification)"
fi
