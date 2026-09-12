if exist ..\done\fwupdate.gz del ..\done\fwupdate.gz
if exist ..\done\firmware_update_html_gz.h del ..\done\firmware_update_html_gz.h

..\binaries\webcompressor -c -f ..\temp\fwupdate.html ..\done\fwupdate.html.gz
..\binaries\binconvert export ..\done\fwupdate.html.gz ..\done\firmware_update_html_gz.h firmware_update_html_gz
del ..\done\fwupdate.html.gz