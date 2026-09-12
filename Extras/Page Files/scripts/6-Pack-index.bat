if exist ..\done\index.html.gz del ..\done\index.html.gz
if exist ..\done\index_gz.h del ..\done\index_gz.h

..\binaries\webcompressor -c -f ..\temp\index.html ..\done\index.html.gz
..\binaries\binconvert export ..\done\index.html.gz ..\done\index_gz.h index_gz
del ..\done\index.html.gz