..\binaries\binconvert import editor_gz.h editor.html.gz
..\binaries\webcompressor -d editor.html.gz
del -y editor.html.gz
del editor_gz.h