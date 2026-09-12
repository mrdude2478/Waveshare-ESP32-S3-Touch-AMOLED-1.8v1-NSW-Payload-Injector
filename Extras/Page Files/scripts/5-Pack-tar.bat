if exist ..\done\tar.html.gz del ..\done\tar.html.gz
if exist ..\done\tar_gz.h del ..\done\tar_gz.h

..\binaries\webcompressor -c -f ..\temp\tar.html ..\done\tar.html.gz
..\binaries\binconvert export ..\done\tar.html.gz ..\done\tar_gz.h tar_gz
del ..\done\tar.html.gz