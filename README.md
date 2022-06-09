# How to keep your plants moist
## An over-engineered chronicle

TODO. I wrote this last summer and it was working perfectly but I messed up the assembly code when I tried to enhance it by adding a battery monitoring service. The plan was to know if the system failed because it ran out of battery before my plants died from *thirst*. It sends an HTTP request to a pushbullet API which then sends me a message on my phone.

The assembly code is to make use of the \~ultra\~ low-power (co) processor (ULP) on the ESP-32 while the main processor sleeps. It keeps my battery high and my plants moist. 

Two lessons from this project:
1. Stay hydrated.
2. Use version control. 
