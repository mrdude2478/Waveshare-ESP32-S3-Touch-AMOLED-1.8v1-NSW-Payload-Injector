..\binaries\binconvert import index_gz.h index.html.gz
..\binaries\webcompressor -d index.html.gz
del -y index.html.gz
del index_gz.h