..\binaries\binconvert import config.h config.html.gz
..\binaries\webcompressor -d config.html.gz
del -y config.html.gz
del config.h