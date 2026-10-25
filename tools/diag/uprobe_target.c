/* uprobe 终极判定靶子：hot_loop_fn 每秒被调用 10 次，持续 120 秒 */
#include <stdio.h>
#include <unistd.h>

__attribute__((noinline)) void hot_loop_fn(int counter) {
    /* 防内联；做点小事让调用可观测 */
    if (counter % 1000 == 0) printf("tick %d\n", counter);
}

int main(void) {
    printf("uprobe_target pid=%d\n", getpid());
    for (int i = 0; i < 1200; i++) {
        hot_loop_fn(i);
        usleep(100000);
    }
    return 0;
}
