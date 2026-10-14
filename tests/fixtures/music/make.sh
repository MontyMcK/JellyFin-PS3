#!/bin/bash
# The music fixtures of tests/test_local_tags.cpp and tests/test_local_audio.cpp, made with ffmpeg
# (run here; the results are committed, the tests do not need ffmpeg).
#
# Every file carries the same signal, so a test can compare any decode with the formula:
#   left  = 0.5 sin(2 pi (300 t + 150 t^2))      right = 0.5 sin(2 pi (500 t + 100 t^2))
# The frequency rises with time, so a wrong position is a wrong sound.
set -e
cd "$(dirname "$0")"
Q="-hide_banner -loglevel error -y"
STEREO="aevalsrc=0.5*sin(2*PI*(300*t+150*t*t))|0.5*sin(2*PI*(500*t+100*t*t))"
MONO="aevalsrc=0.5*sin(2*PI*(300*t+150*t*t))"

ffmpeg $Q -f lavfi -i "color=c=red:s=16x16" -frames:v 1 cover.jpg

# FLAC: tagged with a cover (44.1 kHz, so it is resampled), plain 48 kHz, 24-bit 96 kHz, 5.1
ffmpeg $Q -f lavfi -i "$STEREO:s=44100:d=2" -i cover.jpg -map 0:a -map 1:v -c:a flac -sample_fmt s16 -c:v copy -disposition:v attached_pic \
    -metadata title="Chirp ☃ Test" -metadata artist="A. Tester" -metadata album="Fixtures" -metadata track=3/10 -metadata disc=1/2 \
    chirp44.flac
ffmpeg $Q -f lavfi -i "$STEREO:s=48000:d=2" -c:a flac -sample_fmt s16 chirp48.flac
ffmpeg $Q -f lavfi -i "$STEREO:s=96000:d=0.5" -c:a flac -sample_fmt s32 -bits_per_raw_sample 24 chirp96_24.flac
ffmpeg $Q -f lavfi -i "aevalsrc=0.2*sin(2*PI*300*t)|0.2*sin(2*PI*400*t)|0.2*sin(2*PI*500*t)|0.2*sin(2*PI*600*t)|0.2*sin(2*PI*700*t)|0.2*sin(2*PI*800*t):s=48000:d=0.5:c=5.1" \
    -c:a flac -sample_fmt s16 surround6.flac

# MP3: ID3v2.4 with a cover and a Xing / LAME header; ID3v2.3; ID3v1 only without a Xing header; mono MPEG 2
ffmpeg $Q -f lavfi -i "$STEREO:s=44100:d=2" -i cover.jpg -map 0:a -map 1:v -c:a libmp3lame -b:a 128k -c:v copy -id3v2_version 4 \
    -metadata title="Chirp ☃ Test" -metadata artist="A. Tester" -metadata album="Fixtures" -metadata track=3/10 -metadata disc=1/2 \
    -metadata:s:v title="Cover" -metadata:s:v comment="Cover (front)" chirp44_v24.mp3
ffmpeg $Q -f lavfi -i "$STEREO:s=44100:d=2" -i cover.jpg -map 0:a -map 1:v -c:a libmp3lame -b:a 128k -c:v copy -id3v2_version 3 \
    -metadata title="Chirp v2.3" -metadata artist="B. Tester" -metadata album="Old Tags" -metadata track=7 \
    -metadata:s:v comment="Cover (front)" chirp44_v23.mp3
ffmpeg $Q -f lavfi -i "$STEREO:s=44100:d=2" -c:a libmp3lame -b:a 128k -write_xing 0 -write_id3v2 0 -write_id3v1 1 \
    -metadata title="Plain" -metadata artist="C. Tester" -metadata album="Tiny" -metadata track=5 plain_v1.mp3
ffmpeg $Q -f lavfi -i "$MONO:s=22050:d=1" -c:a libmp3lame -b:a 32k -ac 1 mono22.mp3
# silence at both ends, no LAME header: what the engine's gapless handover trims (tests/test_music_engine.cpp)
ffmpeg $Q -f lavfi -i "aevalsrc=if(between(t\,0.2\,0.8)\,0.5*sin(2*PI*440*t)\,0)|if(between(t\,0.2\,0.8)\,0.5*sin(2*PI*440*t)\,0):s=44100:d=1" \
    -c:a libmp3lame -b:a 128k -write_xing 0 -write_id3v2 0 -write_id3v1 0 silence_pad.mp3

ls -l *.flac *.mp3 cover.jpg
