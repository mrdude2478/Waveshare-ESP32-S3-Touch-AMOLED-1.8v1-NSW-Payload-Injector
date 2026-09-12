if exist ..\done\filemanager.html.gz ..\done\filemanager.html.gz
if exist ..\done\filemanager.h del ..\done\filemanager.h

..\binaries\webcompressor -c -f ..\temp\filemanager.html ..\done\filemanager.html.gz
..\binaries\binconvert export ..\done\filemanager.html.gz ..\done\filemanager.h filemanager
del ..\done\filemanager.html.gz