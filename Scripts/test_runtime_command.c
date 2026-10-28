#include <assert.h>
#include <stdio.h>
#include "RuntimeCommand.h"
int main(void) {
    char *args[16];
    assert(app_runtime("apple.container")==APP_RUNTIME_APPLE);
    assert(app_runtime("docker-user-501")==APP_RUNTIME_DOCKER);
    assert(app_runtime("wsl")==APP_RUNTIME_UNKNOWN);
    assert(!strcmp(app_runtime_executable(APP_RUNTIME_WSLC,1),"wslc.exe"));
    assert(!app_runtime_executable(APP_RUNTIME_APPLE,1));
    size_t n=app_runtime_exec(APP_RUNTIME_DOCKER,"docker","unix:///tmp/docker.sock","0","test","/helper",args,16);
    assert(n==10 && !strcmp(args[1],"--host") && !strcmp(args[5],"--user") && args[n]==NULL);
    n=app_runtime_exec(APP_RUNTIME_APPLE,"container","","","test","/helper",args,16);
    assert(n==6 && !strcmp(args[1],"exec") && !strcmp(args[2],"-i") && !strcmp(args[3],"test"));
    n=app_runtime_exec(APP_RUNTIME_WSLC,"C:\\Program Files\\WSL\\wslc.exe","","0","test","/helper",args,16);
    assert(n==8 && !strcmp(args[2],"--interactive") && !strcmp(args[4],"0"));
    assert(!app_runtime_exec(APP_RUNTIME_APPLE,"container","unix:///tmp/docker.sock","","test","/helper",args,16));
    assert(!app_runtime_exec(APP_RUNTIME_DOCKER,"docker","","","test","/helper",args,2));
    puts("PASS Docker, Apple Container, and Windows wslc argument contracts");
}
