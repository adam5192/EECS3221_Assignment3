# EECS3221_Assignment3

compile by typing 'make' in terminal



Start_Alarm(100): Group(1) 5 Hello world

Change_Alarm(100): Group(1) 10 Updated message

Cancel_Alarm(100)

Suspend_Alarm(100)

Reactivate_Alarm(100)

View_Alarms

**Temporary Tests for Change_Alarm:
**
Start_Alarm(123): Group(1) 10 Hello world
Change_Alarm(123): Group(2) 20 Updated message 
    Timestamp should update in message
Change_Alarm(999): Group(2) 20 Nothing to change
    Should fail (no alarm with id 999)
