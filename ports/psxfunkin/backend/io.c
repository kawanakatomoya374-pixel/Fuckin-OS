/* PSXFunkin on C-OS: file access and the overlay data stream. The game reads "\\CHAR\\BFDEAD.ARC;1" style names from a disc; here a name is looked up in
 * FILES.TXT (made by tools/prep_assets.py from funkin.xml: "<iso name> <file below the data directory>") and read with stdio. A stage / the menu starts with
 * Overlay_Load("\\WEEK1\\WEEK1.EXE;1"): the game then takes its textures one after the other with Overlay_DataRead(), in the order OVERLAYS.TXT lists them
 * ("<overlay name>: file file ..."). The code of an overlay is linked into the program, only its data stream is emulated. */
#include "io.h"
#include "mem.h"
#include "audio.h"
#include "main.h"
#include "../plat.h"

#define MAX_FILES 1024
#define MAX_OVERLAYS 16
#define MAX_OVL_FILES 32

static struct { char name[40]; char path[96]; } files[MAX_FILES]; static int nfiles;
static struct { char name[40]; char path[MAX_OVL_FILES][96]; int n; } ovl[MAX_OVERLAYS]; static int novl;
static int cur_ovl = -1, cur_pos;

static void norm(const char *in, char *out, size_t cap)            /* "\CHAR\BFDEAD.ARC;1" -> "char/bfdead.arc" */
{
	size_t n = 0; if (*in == '\\' || *in == '/') in++;
	for (; *in && *in != ';' && n + 1 < cap; ++in) { char c = *in == '\\' ? '/' : *in; if (c >= 'A' && c <= 'Z') c = (char)(c + 32); out[n++] = c; }
	out[n] = 0;
}
static char *read_text(const char *rel)
{
	char full[256]; snprintf(full, sizeof full, "%s/%s", plat_data_dir(), rel);
	FILE *f = fopen(full, "rb"); if (!f) return NULL;
	fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
	char *b = (char*)malloc((size_t)n + 1); if (!b) { fclose(f); return NULL; }
	size_t rd = fread(b, 1, (size_t)n, f); fclose(f); b[rd] = 0; return b;
}

void IO_Init(void)
{
	nfiles = novl = 0;
	char *t = read_text("FILES.TXT");
	if (t) {
		for (char *l = t; l && *l; ) {
			char *nl = strchr(l, '\n'); if (nl) *nl = 0;
			char *sp = strchr(l, ' ');
			if (sp && nfiles < MAX_FILES) { *sp = 0; snprintf(files[nfiles].name, sizeof files[0].name, "%s", l); snprintf(files[nfiles].path, sizeof files[0].path, "%s", sp + 1); size_t k = strlen(files[nfiles].path); if (k && files[nfiles].path[k - 1] == '\r') files[nfiles].path[k - 1] = 0; nfiles++; }
			l = nl ? nl + 1 : NULL;
		}
		free(t);
	}
	t = read_text("OVERLAYS.TXT");
	if (t) {
		for (char *l = t; l && *l; ) {
			char *nl = strchr(l, '\n'); if (nl) *nl = 0;
			char *colon = strchr(l, ':');
			if (colon && novl < MAX_OVERLAYS) {
				*colon = 0; snprintf(ovl[novl].name, sizeof ovl[0].name, "%s", l); ovl[novl].n = 0;
				for (char *tok = strtok(colon + 1, " \r"); tok && ovl[novl].n < MAX_OVL_FILES; tok = strtok(NULL, " \r")) snprintf(ovl[novl].path[ovl[novl].n++], sizeof ovl[0].path[0], "%s", tok);
				novl++;
			}
			l = nl ? nl + 1 : NULL;
		}
		free(t);
	}
	printf("[psxf] %d files, %d overlays under %s\n", nfiles, novl, plat_data_dir());
}
void IO_Quit(void) { }

static const char *lookup(const char *iso)
{
	char key[48]; norm(iso, key, sizeof key);
	for (int i = 0; i < nfiles; ++i) if (!strcmp(files[i].name, key)) return files[i].path;
	return NULL;
}
const char *PSXF_LookupFile(const char *iso) { return lookup(iso); }

void IO_FindFile(CdlFILE *file, const char *path)
{
	Audio_StopMus();
	if (!lookup(path)) { sprintf(error_msg, "[IO_FindFile] %s not found", path); ErrorLock(); }
	norm(path, file->path, sizeof file->path);
}
void IO_SeekFile(CdlFILE *file) { (void)file; Audio_StopMus(); }

static IO_Data read_file(const char *rel)
{
	char full[256]; snprintf(full, sizeof full, "%s/%s", plat_data_dir(), rel);
	FILE *f = fopen(full, "rb"); if (!f) { sprintf(error_msg, "[IO_Read] cannot open %.200s", full); ErrorLock(); return NULL; }
	fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
	u8 *b = (u8*)Mem_Alloc((size_t)n + 2048); if (!b) { fclose(f); sprintf(error_msg, "[IO_Read] out of memory (%ld)", n); ErrorLock(); return NULL; }
	size_t rd = fread(b, 1, (size_t)n, f); fclose(f); memset(b + rd, 0, 2048);
	return (IO_Data)b;
}
IO_Data IO_ReadFile(CdlFILE *file)
{
	const char *rel = lookup(file->path); if (!rel) { sprintf(error_msg, "[IO_ReadFile] %s not found", file->path); ErrorLock(); }
	return read_file(rel);
}
IO_Data IO_Read(const char *path)
{
	const char *rel = lookup(path); if (!rel) { sprintf(error_msg, "[IO_Read] %s not found", path); ErrorLock(); }
	Audio_StopMus();
	return read_file(rel);
}

void Overlay_Load(const char *path)
{
	char key[48]; norm(path, key, sizeof key); cur_ovl = -1;
	for (int i = 0; i < novl; ++i) if (!strcmp(ovl[i].name, key)) cur_ovl = i;
	if (cur_ovl < 0) { sprintf(error_msg, "[Overlay_Load] %s is not in OVERLAYS.TXT", path); ErrorLock(); }
	Audio_StopMus(); cur_pos = 0;
}
void Overlay_DataInit(void) { cur_pos = 0; }
IO_Data Overlay_DataRead(void)
{
	if (cur_ovl < 0 || cur_pos >= ovl[cur_ovl].n) { sprintf(error_msg, "[Overlay_DataRead] no more data in %s", cur_ovl < 0 ? "?" : ovl[cur_ovl].name); ErrorLock(); }
	return read_file(ovl[cur_ovl].path[cur_pos++]);
}
