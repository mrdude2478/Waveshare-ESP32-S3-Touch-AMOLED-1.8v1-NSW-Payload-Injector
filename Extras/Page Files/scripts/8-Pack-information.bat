if exist ..\done\information.html.gz del ..\done\information.html.gz
if exist ..\done\info_gz.h del ..\done\info_gz.h

..\binaries\webcompressor -c -f ..\temp\information.html ..\done\information.html.gz
..\binaries\binconvert export ..\done\information.html.gz ..\done\info_gz.h info_gz
del ..\done\information.html.gz