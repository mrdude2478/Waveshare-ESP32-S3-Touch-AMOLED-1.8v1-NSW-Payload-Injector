if exist ..\done\editor.html.gz del ..\done\editor.html.gz
if exist ..\done\editor_gz.h del ..\done\editor_gz.h

..\binaries\webcompressor -c -f ..\temp\editor.html ..\done\editor.html.gz
..\binaries\binconvert export ..\done\editor.html.gz ..\done\editor_gz.h editor_gz
del ..\done\editor.html.gz