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

if [ $# -lt 2 ]; then
    echo "use: $0 SERIEDIR DESTDIR"
    exit 1
fi

SDIR=$1
DDIR=$2

if [ ! -f "$SDIR/series" ]; then
    echo "missing $SDIR/series file"
    exit 2
fi

FILES=`cat $SDIR/series | xargs`

if [ ! -d "$DDIR" ]; then
    echo "invalid destination directory $DDIR"
    exit 3
fi

for p in $FILES; do
    CFILE=$SDIR/$p
    echo ">>> applying $p"
    patch -d $DDIR -p1 < $CFILE
done

