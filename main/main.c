#include "system_tasks.h"

void app_main(void) 
{
  /* Init System-Level Tasks (motor, sensors, webserver, etc) */
  system_tasks_init();

  /* Start System-Level Tasks (motor, sensors, webserver, etc) */
  system_tasks_start();
}
