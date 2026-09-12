if exist ..\done\styles.css.gz del ..\done\styles.css.gz
if exist ..\done\css_gz.h del ..\done\css_gz.h

..\binaries\webcompressor -c -f ..\temp\styles.css ..\done\styles.css.gz
..\binaries\binconvert export ..\done\styles.css.gz ..\done\css_gz.h css_gz
del ..\done\styles.css.gz