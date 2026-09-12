if exist ..\done\config.html.gz del ..\done\config.html.gz
if exist ..\done\config.h del ..\done\config.h

..\binaries\webcompressor -c -f ..\temp\config.html ..\done\config.html.gz
..\binaries\binconvert export ..\done\config.html.gz ..\done\config.h config_html_gz
del ..\done\config.html.gz