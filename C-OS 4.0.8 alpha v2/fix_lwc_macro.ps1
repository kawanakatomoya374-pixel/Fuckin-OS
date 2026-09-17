$path = 'C:\Users\cs02z\Downloads\C-OS_4_0_8_alpha_netsurf_quickjs (1)\cos\src\third_party\netsurf-all-3.11\libwapcaplet\include\libwapcaplet\libwapcaplet.h'
$lines = Get-Content -LiteralPath $path
$start = 159
$replacement = @(
"#define lwc_string_unref(str) { \\",
"\tlwc_string *__lwc_s = (str); \\",
"\tif (__lwc_s != NULL) { \\",
"\t\t__lwc_s->refcnt--; \\",
"\t\tif ((__lwc_s->refcnt == 0) || \\",
"\t\t    ((__lwc_s->refcnt == 1) && (__lwc_s->insensitive == __lwc_s))) \\",
"\t\t\tlwc_string_destroy(__lwc_s); \\",
"\t} \\",
"\t}"
)
for ($i = 0; $i -lt $replacement.Count; $i++) {
    $lines[$start + $i] = $replacement[$i]
}
Set-Content -LiteralPath $path -Value $lines -Encoding utf8
Write-Output 'FIX_APPLIED'
