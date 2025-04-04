# EECS3221 Assignment 3 – Alarm System

This project implements a multi-threaded alarm management system using POSIX threads, unnamed semaphores, and circular buffers.

## 📦 Compilation

To compile the program, simply run:

```bash
make
```

## 🧪 Sample Usage

Enter alarm requests in the following formats:

```
Start_Alarm(<Alarm_ID>): Group(<Group_ID>) <Interval> <Message>
Change_Alarm(<Alarm_ID>): Group(<Group_ID>) <Interval> <Updated Message>
Cancel_Alarm(<Alarm_ID>)
Suspend_Alarm(<Alarm_ID>)
Reactivate_Alarm(<Alarm_ID>)
View_Alarms
```

### 🧪 Example Inputs

```
Start_Alarm(100): Group(1) 5 Hello world
Change_Alarm(100): Group(1) 10 Updated message
Cancel_Alarm(100)
Suspend_Alarm(100)
Reactivate_Alarm(100)
View_Alarms
```

---

### 🔧 Temporary Tests for Change_Alarm

```
Start_Alarm(123): Group(1) 10 Hello world
Change_Alarm(123): Group(2) 20 Updated message
    ✅ Timestamp should update in message

Change_Alarm(999): Group(2) 20 Nothing to change
    ❌ Should fail (no alarm with ID 999)
```

---
