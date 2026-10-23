/* cos_media.c - single-instance launcher for Media.c-os. */
#include "types.h"
#include "serial.h"
#include "task.h"
#include "cos_app_window.h"
#include "cos_media_elf.h"
extern int64_t cos_launch_elf_image_args(const char *name, const unsigned char *image, unsigned int image_len,
                                         const char *const *argv, int argc, const char *const *envp, int envc);
static uint32_t s_pid;
static char s_pending[256];
static bool alive(void){process_t *p=s_pid?process_get_by_pid(s_pid):NULL;return p&&p->state!=TASK_ZOMBIE&&p->state!=TASK_UNUSED;}
void cos_media_open(const char *path){
    bool has=path&&path[0]&&!(path[0]=='/'&&path[1]=='\0');
    if(alive()){
        if(cos_app_window_deliver_path(s_pid,has?path:"",COS_EV_OPEN,0,0))return;
        if(has){size_t n=0;while(path[n]&&n<sizeof(s_pending)-1){s_pending[n]=path[n];n++;}s_pending[n]='\0';}
        return;
    }
    const char *av[2]={"Media.c-os",has?path:NULL};int64_t p=cos_launch_elf_image_args("Media.c-os",cos_media_elf,cos_media_elf_len,av,has?2:1,NULL,0);s_pid=p>0?(uint32_t)p:0;s_pending[0]='\0';serial_puts(p>0?"[MEDIA] launched\n":"[MEDIA] launch FAILED\n");
}
void cos_media_on_window_created(uint32_t pid){if(pid==s_pid&&s_pending[0]){cos_app_window_deliver_path(pid,s_pending,COS_EV_OPEN,0,0);s_pending[0]='\0';}}
const unsigned char *cos_media_image(unsigned int *len){if(len)*len=cos_media_elf_len;return cos_media_elf;}
