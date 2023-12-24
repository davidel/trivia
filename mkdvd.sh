#!/bin/sh
#    Copyright 2023 Davide Libenzi
# 
#    Licensed under the Apache License, Version 2.0 (the "License");
#    you may not use this file except in compliance with the License.
#    You may obtain a copy of the License at
# 
#        http://www.apache.org/licenses/LICENSE-2.0
# 
#    Unless required by applicable law or agreed to in writing, software
#    distributed under the License is distributed on an "AS IS" BASIS,
#    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
#    See the License for the specific language governing permissions and
#    limitations under the License.
# 

FFMPEG=ffmpeg-nf
DEBUG=0
IRES="720x480"
ASP="16:9"
IPAD=1
KASP=1
BURN=0
MKDVD=0
CLEAN=0
DRYRUN=0
TITLE=""
ACP=""
DVDDEV="/dev/hda"

#
# Max size of MPG in KB (~3.7GB). Less than full DVD capacity
# because FFMPEG does not strictly respect the bps we ask, and may
# lead to overflow.
#
MAXB=3700000


usage()
{
    printf "Usage: %s [-DBvNuCWadXYAVxyMT] INFILE\n" $(basename $1) >&2
    printf "\t-D         Sets debug mode\n" >&2
    printf "\t-B         Burn DVD (implies -v)\n" >&2
    printf "\t-C         Clean temporary files when done\n" >&2
    printf "\t-u         Dry run. Just detect the configuration and show FFMEPG options\n" >&2
    printf "\t-v         Create DVD directory structure\n" >&2
    printf "\t-N         Do not PAD the image to bring to resolution\n" >&2
    printf "\t-d DEV     Sets the DVD device (%s)\n" "$DVDDEV" >&2
    printf "\t-T TITLE   Sets title for the DVD\n" >&2
    printf "\t-M SIZE    Sets maximum size for the DVD in KB (%s KB)\n" $MAXB >&2
    printf "\t-a NACHAN  Sets the number of audio channels\n" >&2
    printf "\t-A PHASP   Sets DVD physical aspect ratio (%s)\n" $ASP >&2
    printf "\t-r IRES    Sets the output pixel resolution (%s)\n" $IRES >&2

    exit 1
}

dbg()
{
    if [ $DEBUG -ne 0 ]; then
        echo $1
    fi
}

round2()
{
    expr \( $1 / 2 \) \* 2
}

toint()
{
    echo -n $1 | cut -d '.' -f 1
}

kvalue()
{
    KV1=`echo $1 | cut -d $2 -f 1`
    KV2=`echo $1 | cut -d $2 -f 2`
    echo "scale = 6; $KV1 / $KV2" | bc -l
}


#
# Here the start of all ...
#
while getopts 'DBvNuCa:d:A:M:T:r:' OPTION
do
    case $OPTION in
        D)
            DEBUG=1
            ;;
        B)
            MKDVD=1
            BURN=1
            ;;
        C)
            CLEAN=1
            ;;
        N)
            IPAD=0
            ;;
        u)
            DRYRUN=1
            ;;
        v)
            MKDVD=1
            ;;
        M)
            MAXB=`expr $OPTARG \* 1024`
            ;;
        A)
            ASP="$OPTARG"
            ;;
        r)
            IRES="$OPTARG"
            ;;
        T)
            TITLE="$OPTARG"
            ;;
        a)
            ACP="-ac $OPTARG"
            ;;
        d)
            DVDDEV="$OPTARG"
            ;;
        ?)
            usage $0
            ;;
    esac
done
shift $(($OPTIND - 1))

if [ $# -lt 1 ]; then
    usage $0
fi

INFILE=$1

if [ ! -f "$INFILE" ]; then
    echo "AVI file $INFILE not found"
    exit 2
fi

if [ ! -b "$DVDDEV" ]; then
    echo "Invalid DVD device $DVDDEV"
    exit 3
fi
DMOUNT=`mount | grep "$DVDDEV"`
if [ "$DMOUNT" != "" ]; then
    echo "Device $DVDDEV is mounted!"
    exit 3
fi

IMGFMT=
if [ $ASP != "0" ]; then
    if [ $(echo -n $ASP | grep -E -q '[0-9]+:[0-9]+$'; echo -n $?) -gt 0 ]; then
        echo "Invalid aspect ratio format $ASP"
        exit 4
    fi

#
# Calculate aspect ratio constant
#
    KASP=$(kvalue $ASP ":")

    IMGFMT="-aspect $ASP"
fi
dbg "Physical Aspect Ratio: $KASP"

OXRES=`echo $IRES | cut -d 'x' -f 1`
OYRES=`echo $IRES | cut -d 'x' -f 2`
OIASP=`echo "scale = 6; $OXRES / $OYRES" | bc -l`
dbg "Pixel Aspect Ratio: $OIASP"

KVA=`echo "scale = 6; $OIASP / $KASP" | bc -l`
dbg "Aspect Ratio Ratio: $KVA"

dbg "Input File: $INFILE"

FNAME=`basename $INFILE`
BNAME=`echo -n $FNAME | sed -r -e 's/^([^.]+)\..*$/\1/'`

OUTFILE=$BNAME.mpg
ODIR=__DVD__.$BNAME

dbg "Output File: $OUTFILE"

if [ "$TITLE" == "" ]; then
    TITLE=$BNAME
fi

MINFO=`"$FFMPEG" -i "$INFILE" 2>&1 | tr '\n' '|'`

MOVLEN=`echo $MINFO | tr '|' '\n' | grep Duration | egrep -o '[0-9]{2}:[0-9]{2}'`
MHOUR=`echo $MOVLEN | cut -d ':' -f 1`
MMIN=`echo $MOVLEN | cut -d ':' -f 2`
MSEC=`expr 60 \* $MMIN \+ 3600 \* $MHOUR`

dbg "Movie Duration: $MHOUR:$MMIN:$MSEC"

KBS=`expr $MAXB / $MSEC`
KBPS=`expr $KBS \* 1024 \* 8`

dbg "Output Rate: $KBPS bps"

MRES=`echo $MINFO | tr '|' '\n' | grep Video | egrep -o '[0-9]+x[0-9]+'`
XMRES=`echo $MRES | cut -d 'x' -f 1`
YMRES=`echo $MRES | cut -d 'x' -f 2`

dbg "Movie Input Resolution: ${XMRES}x${YMRES}"

#
# Get Display Aspect Ratio and Pixel Aspect Ratio
#
DAR=`echo $MINFO | egrep -o 'DAR +[0-9]+:[0-9]+' | sed -r -e 's/DAR *([0-9]+:[0-9]+)$/\1/g'`
if [ "$DAR" != "" ]; then
    dbg "Movie Display Aspect Ratio: $DAR"

    KDAR=$(kvalue $DAR ":")
fi

PAR=`echo $MINFO | egrep -o 'PAR +[0-9]+:[0-9]+' | sed -r -e 's/PAR *([0-9]+:[0-9]+)$/\1/g'`
if [ "$PAR" != "" ]; then
    dbg "Movie Pixel Aspect Ratio: $PAR"

    KPAR=$(kvalue $PAR ":")
fi

IMGASP=`echo "scale = 6; $XMRES / $YMRES" | bc -l`

#
# Get integer approximations
#
SIA=$(toint `echo "$IMGASP * 100000" | bc -l`)
OIA=$(toint `echo "$OIASP * 100000" | bc -l`)

if [ $SIA -ge $OIA ]; then
    NXMRES=$OXRES
    NYMRES=`echo "scale = 6; ( $NXMRES / $IMGASP ) / $KVA" | bc -l`
    NYMRES=$(round2 $(toint $NYMRES))
else
    NYMRES=$OYRES
    NXMRES=`echo "scale = 6; $NYMRES * $IMGASP * $KVA" | bc -l`
    NXMRES=$(round2 $(toint $NXMRES))
fi

dbg "Movie Output Resolution: ${NXMRES}x${NYMRES}"

IMGPAD=
NMRES="${NXMRES}x${NYMRES}"

dbg "Movie Output Aspect Ratio Resolution: ${OXRES}x${OYRES}"

if [ $IPAD -ne 0 -a $ASP != "0" ]; then
    if [ $OYRES -ge $NYMRES ]; then
        YPAD=`echo "($OYRES - $NYMRES) / 2" | bc -l`
        YPAD=$(round2 $(toint $YPAD))
        RYPAD=`expr $OYRES \- $NYMRES \- $YPAD`
        IMGPAD="$IMGPAD -padtop $YPAD -padbottom $RYPAD"

        dbg "Movie Padding (Top - Bottom): $YPAD - $RYPAD"
    elif [ $OXRES -ge $NXMRES ]; then
        XPAD=`echo "($OXRES - $NXMRES) / 2" | bc -l`
        XPAD=$(round2 $(toint $XPAD))
        RXPAD=`expr $OXRES \- $NXMRES \- $XPAD`
        IMGPAD="$IMGPAD -padleft $XPAD -padright $RXPAD"

        dbg "Movie Padding (Left - Right): $XPAD - $RXPAD"
    fi
fi

FFMPEGOPTS="-target ntsc-dvd -vsync 1 -async 10000 -b $KBPS $IMGFMT -s $NMRES $IMGPAD $ACP"

if [ $DRYRUN -ne 0 ]; then
    dbg "FFMPEG options: $FFMPEGOPTS"
    exit 1
fi

if [ ! -f "$OUTFILE" ]; then
    dbg "Running FFMPEG with: $FFMPEGOPTS"
    nice -n 19 "$FFMPEG" -i "$INFILE" $FFMPEGOPTS "$OUTFILE"
    if [ $? -ne 0 ]; then
        rm "$OUTFILE"
        exit 4
    fi
fi

if [ $MKDVD -ne 0 -a ! -d "$ODIR" ]; then
    dbg "Creating DVD Directory Structure: $ODIR"
    mkdir "$ODIR"
    nice -n 19 dvdauthor --title -f "$OUTFILE" -o "$ODIR"
    if [ $? -ne 0 ]; then
        rm -rf "$ODIR"
        exit 5
    fi
    dvdauthor -T -o "$ODIR"
    if [ $? -ne 0 ]; then
        rm -rf "$ODIR"
        exit 6
    fi
fi

if [ $BURN -ne 0 ]; then
    if [ ! -d "$ODIR" ]; then
        echo "DVD structure directory does not exist: $ODIR"
        exit 7
    fi
    growisofs -Z "$DVDDEV" -V "$TITLE" -dvd-video "$ODIR"
    if [ $? -ne 0 ]; then
        exit 8
    fi
fi

if [ $CLEAN -ne 0 ]; then
    rm -rf "$OUTFILE" "$ODIR"
fi

