const fs = require('fs');
const path = 'src/third_party/netsurf-all-3.11/libwapcaplet/include/libwapcaplet/libwapcaplet.h';
let text = fs.readFileSync(path, 'utf8');
const start = text.indexOf('#define lwc_string_unref');
if (start === -1) {
  console.error('ERROR: start marker not found');
  process.exit(1);
}
const end = text.indexOf('/**', start);
if (end === -1) {
  console.error('ERROR: end marker not found');
  process.exit(1);
}
const replacement = `#define lwc_string_unref(str) {                        \\
\t\tlwc_string *__lwc_s = (str);                \\
\t\tif (__lwc_s != NULL) {                    \\
\t\t\t__lwc_s->refcnt--;                    \\
\t\t\tif ((__lwc_s->refcnt == 0) ||                    \\
\t\t\t    ((__lwc_s->refcnt == 1) && (__lwc_s->insensitive == __lwc_s)))   \\
\t\t\t\tlwc_string_destroy(__lwc_s);             \\
\t\t}                                       \\
\t}\n`;
text = text.slice(0, start) + replacement + text.slice(end);
fs.writeFileSync(path, text, 'utf8');
console.log('FIXED');
