..\binaries\binconvert import firmware_update_html_gz.h fwupdate.html.gz
..\binaries\webcompressor -d fwupdate.html.gz
del -y fwupdate.html.gz
del firmware_update_html_gz.h