#include "soc/rtc_cntl_reg.h"
#include "soc/soc_ulp.h"
#include "soc/rtc_io_reg.h"

	/* GPIO34 */
	.set adc_channel_3, 6 +1
	/* GPIO33 */
	.set adc_channel_1, 5 +1
	/* GPIO32 */
	.set adc_channel_2, 4 +1

	.set dryness_high_thr, 150

	/* this is about 2.8 volts */
	.set low_battery_thr, 511

	/* 7.95 hours */
	.set min_moisture_interval, 0xFFF0

	/* 1 hour */
	.set min_battery_interval, 0x202F

	/* Configure the number of ADC samples to average on each measurement.
	   For convenience, make it a power of 2. */
    /* take 32 measurements */
	.set adc_oversampling_factor_log, 5
	.set adc_oversampling_factor, (1 << adc_oversampling_factor_log)

	/* Define variables, which go into .bss section (zero-initialized data) */
	.bss

	.global channel_1_last_result
channel_1_last_result:
	.long 0

	.global channel_2_last_result
channel_2_last_result:
	.long 0

	.global channel_3_last_result
channel_3_last_result:
	.long 0

	.global debug_1
debug_1:
	.long 0


	.global debug_2
debug_2:
	.long 0



/* last_moisture_notification_time, last_battery_notification_time, current_time
	are all TIME variables. TIME variables have the following structure:

	TIME[0] = RTC_CNTL_TIME0_REG[31:16]
	TIME[1] = RTC_CNTL_TIME1_REG[15:0]
*/
	.global last_moisture_notification_time
last_moisture_notification_time:
	.long 0
	.long 0

	.global last_battery_notification_time
last_battery_notification_time:
	.long 0
	.long 0

	.global critical_battery_flag
critical_battery_flag:
	.long 0

	.global current_time
current_time:
	.long 0
	.long 0

temp_res:
	.long 0
temp_arg:
	.long 0
	.long 0
	.long 0
	.long 0

temp:
	.long 0
	.long 0

	/* Code goes into .text section */
	.text
	.global entry
entry:
	/* do measurements using ADC */
	/* r0,r1,r2 will be used as accumulator */
	move r0, 0
	move r1, 0
	move r2, 0

	/* initialize the loop counter */
	stage_rst
measure:
	/* measure and add value to accumulator */
	adc r3, 0, adc_channel_1  
	add r0, r0, r3
	adc r3, 0, adc_channel_2  
	add r1, r1, r3
	adc r3, 0, adc_channel_3  
	add r2, r2, r3
	/* increment loop counter and check exit condition */
	stage_inc 1
	jumps measure, adc_oversampling_factor, lt

	/* divide accumulator by adc_oversampling_factor.
	   Since it is chosen as a power of two, use right shift */
	rsh r0, r0, adc_oversampling_factor_log
	rsh r1, r1, adc_oversampling_factor_log
	rsh r2, r2, adc_oversampling_factor_log
	/* averaged value is now in r0; store it into last_result */
	move r3, channel_1_last_result
	st r0, r3, 0
	move r3, channel_2_last_result
	st r1, r3, 0
	move r3, channel_3_last_result
	st r2, r3, 0

	/* compare with low_battery_thr; send warning if battery voltage < low_battery_thr */
	move r3, low_battery_thr
	sub r0, r2, r3
	jump send_battery_notif, ov

	/* compare with dryness_high_thr; send notification if dryness (voltage) > dryness_high_thr */
/*	move r3, dryness_high_thr
	sub r0, r3, r0
	jump send_moisture_notif, ov
	sub r0, r3, r1
	jump send_moisture_notif, ov
*/
	/* value within range, end the program */
	.global exit
exit:
	halt

send_moisture_notif:
	move r3, check_last_moisture_notif_time
	jump read_current_time
check_last_moisture_notif_time:
/* 
   Call check_time_greater_than_min_interval
*/
	move r3, temp_arg
	move r2, last_moisture_notification_time
	st r2, r3, 0
	move r2, min_moisture_interval
	st r2, r3, 2
	move r2, process_time_check_moisture
	st r2, r3, 4
	jump check_time_greater_than_min_interval
process_time_check_moisture:
	move r0, temp_res
	/* prepare r3 for update_and_wake */
	move r3, last_moisture_notification_time
	jumpr update_and_wake, 1, eq
	halt

/* Assumes last_moisture_notification_time or last_battery_notification_time are in r3*/
update_and_wake:
	/* Update last_notification_time with current_time */
	move r2, current_time
	ld r0, r2, 2
	st r0, r3, 2
	ld r0, r2, 0
	st r0, r3, 0

	.global wake_up
wake_up:
	/* Check if the system can be woken up */
	READ_RTC_FIELD(RTC_CNTL_LOW_POWER_ST_REG, RTC_CNTL_RDY_FOR_WAKEUP)
	and r0, r0, 1
	/* retry until ready bit is set */
	jump wake_up, eq 

	/* Wake up the SoC, end program */
	wake
	/* Stop the ULP timer */
	WRITE_RTC_FIELD(RTC_CNTL_STATE0_REG, RTC_CNTL_ULP_CP_SLP_TIMER_EN, 0)
	halt

send_battery_notif:
	move r3, check_last_battery_notif_time
	jump read_current_time
check_last_battery_notif_time:
/* 
   Call check_time_greater_than_min_interval
*/
	move r3, debug_1
	move r2, 1
	st r2, r3, 0

	move r3, temp_arg 
	move r2, last_battery_notification_time
	st r2, r3, 0
	move r2, min_battery_interval
	st r2, r3, 2
	move r2, process_time_check_battery
	st r2, r3, 4
	jump check_time_greater_than_min_interval
process_time_check_battery:
	move r0, temp_res
	jumpr exit, 0, eq
	/* set the critical_battery_flag to tell main_cpu to send notification about battery */
	move r0, critical_battery_flag
	move r1, 1
	st r1, r0, 0
	/* prepare r3 for update_and_wake */
	move r3, last_battery_notification_time
	jump update_and_wake

/* Function to read the current time and store it. Returns to address in r3 afterwards. */
read_current_time:
	move r2, current_time
	/* Update the timer register with the current time. */
	WRITE_RTC_REG(RTC_CNTL_TIME_UPDATE_REG, RTC_CNTL_TIME_UPDATE_S, 1,1)
wait_RTC_update:
	/* Wait until the timer register is loaded and ready */
	READ_RTC_REG(RTC_CNTL_TIME_UPDATE_REG, RTC_CNTL_TIME_VALID_S, 1)
	jumpr wait_RTC_update, 0, eq

	/* read the current time and store it */
	READ_RTC_REG(RTC_CNTL_TIME0_REG, 16, 16)
	st r0, r2, 0
	READ_RTC_REG(RTC_CNTL_TIME1_REG, 0, 16)
	st r0, r2, 2
	jump r3

/* 
   Load last_notification_time address in temp_arg[0]
   Load min_interval in temp_arg[1] 
   Load return address in temp_arg[2] 
*/
check_time_greater_than_min_interval:
	move r3, temp_arg
	/* load last_notification_time address into r3 */
	ld r3, r3, 0
	/* If both 16 bits of last_notification_time are 0, no updates have been done.*/
	ld r0, r3, 0
	ld r1, r3, 2
	or r1, r0, r1
	move r2, debug_2
	st r1, r2, 0
	/* If no notifications have been ever sent, don't need to check if min_time_interval has occured since last time*/
	jump return_true_check_time, eq

	/* Copy last_notification_time into temp */
	move r2, temp
	ld r0, r3, 0
	st r0, r2, 0
	ld r0, r3, 2
	st r0, r2, 2

	/* Check that current_time - last_notification_time > thresh */
	/* First calculate last_notification_time + thresh and store in temp. */

	/* load last_notification_time lower bits into r0*/
	ld r0, r3, 0
	/* load threshold into r2 */
	move r3, temp_arg
	ld r2, r3, 2

	add r0, r0, r2
	/* handle overflow*/
	jump incr_cur_time, OV
check_time_thresh:
	/* Now check current_time > last_notification_time + thresh */
	move r3, current_time
	move r2, temp
	ld r0, r3, 2
	ld r1, r2, 2
	sub r0, r0, r1
	/* If current_time - (last_notification_time + thresh) overflows, return false*/
	jump return_false_check_time, ov
	ld r0, r3, 0
	ld r1, r2, 0
	sub r0, r0, r1
	jump return_false_check_time, ov
	/* No overflow, return true */
	jump return_true_check_time

incr_cur_time:
	move r2, temp
	/* increment higher 16 bits by 1*/
	ld r0, r2, 2
	/* This should never overflow. */
	add r0, r0, 1
	st r0, r2, 2
	jump check_time_thresh

return_true_check_time:
	move r0, 1
	jump return_check_time
return_false_check_time:
	move r0, 0
return_check_time:
	move r2, temp_res
	st r0, r2, 0
	move r3, temp_arg
	ld r0, r3, 4
	jump r0