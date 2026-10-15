# Tracker Firmware — Checklist ตามลำดับ Sheets

เรียงทั้ง 65 รายการตรงกับ Sheets โดยต่อรายละเอียด Implement ในแถวเดียวกัน หมวดซ้ำทุกแถวเพื่อให้คัดลอกและกรองใน Sheets ได้สะดวก

**วิธีใช้:** นำคอลัมน์ `ผ่านบนเครื่องจริง` ไปทำ Checkbox ใน Sheets ช่อง `☐` หมายถึงยังไม่ลงผลรับรองในเอกสารนี้ ไม่ได้แปลว่าทำไม่ได้ `มีแล้ว` หมายถึงมีโค้ด ไม่ได้หมายถึงทดสอบผ่าน ห้ามติ๊กผ่านจาก Build/Host Test อย่างเดียว ส่วน `HW` และ `ภายนอก` ต้องตรวจวงจรหรือ Server/Application จริง

## Checklist และรายละเอียด Implement

| ลำดับ | หมวด | ฟังก์ชัน / รายการตรวจสอบ | วิธีตรวจสอบ | ผลที่คาดหวัง | Implement / ข้อจำกัด / งานต่อ | ผ่านบนเครื่องจริง |
|---|---|---|---|---|---|---|
| 01 | Power | เปิดเครื่อง | จ่ายไฟจากแบตเตอรี่และเปิดอุปกรณ์ | MCU Boot และเข้าสู่กระบวนการทำงานปกติ | มีแล้ว — ทดสอบกับแบตและวงจรจริง โดยเฉพาะช่วงเปิด GNSS | ☐ |
| 02 | Power | Reset / Restart | Restart อุปกรณ์ | ระบบเริ่มทำงานใหม่โดยไม่ค้าง | มีแล้ว — ตรวจ Boot และ Join ตามเงื่อนไข ไม่รับประกันคง Session ทุกชนิด Reset | ☐ |
| 03 | Power | แรงดันระบบ 3.3 V | วัดแรงดัน Power Rail | แรงดันอยู่ในช่วงที่วงจรกำหนด | HW — วัดตอน Boot, เปิด GNSS, ส่ง LoRa และตื่นจาก Sleep โดยเฉพาะช่วงโหลดเปลี่ยน | ☐ |
| 04 | Battery | อ่านแรงดันแบตเตอรี่ | ตรวจค่า ADC/Debug Log | อ่านค่าแรงดันแบตเตอรี่ได้ | มีแล้ว — GPIO2 อ่านครั้งเดียวต่อการตื่น ต้องวัดเทียบมิเตอร์ | ☐ |
| 05 | Battery | คำนวณ Battery Level | เปรียบเทียบแรงดันกับค่าที่ Firmware รายงาน | แสดงระดับแบตเตอรี่ได้อย่างสมเหตุสมผล | มีข้อจำกัด — ประมาณจากกราฟแรงดัน ไม่ใช่วัดประจุสะสม ต้องเทียบหลายระดับแรงดันและโหลด | ☐ |
| 06 | Battery | Low Battery | จำลองแรงดันต่ำกว่า Threshold | ระบบระบุสถานะ Low Battery | มีแล้ว — Commit `4fbc48d`: เข้า ≤15%, ออก ≥20%, เก็บ RTC, ส่ง flag bit 5; รอทดสอบจริงและอัปเดต Server Decoder | ☐ |
| 07 | Battery | การชาร์จแบตเตอรี่ | ต่อแหล่งจ่ายสำหรับชาร์จ | แบตเตอรี่สามารถชาร์จได้ตามปกติ | HW — ทดสอบตามข้อกำหนดวงจรและแบต ไม่ใช่ Firmware ควบคุมการชาร์จ | ☐ |
| 08 | Battery | สถานะการชาร์จ | ตรวจ LED/สถานะวงจร Charger | สามารถแสดงสถานะการชาร์จได้ | HW — ตรวจ LED Charger ปัจจุบัน Firmware ไม่อ่านสถานะชาร์จเข้ามา | ☐ |
| 09 | GNSS/GPS | เปิด GNSS | สั่งเปิด GNSS จาก Firmware | โมดูลเริ่มทำงานและส่งข้อมูลผ่าน UART | มีแล้ว — VEXT active-low, UART1 RX41 ตรวจ NMEA หลังเปิดไฟ | ☐ |
| 10 | GNSS/GPS | Position Fix | ทดสอบในพื้นที่กลางแจ้ง | สามารถรับตำแหน่ง Latitude/Longitude ได้ | มีแล้ว — เทียบตำแหน่งอ้างอิง ไม่รับประกัน Fix ในอาคาร | ☐ |
| 11 | GNSS/GPS | GPS Validity | ตรวจข้อมูล GNSS | Firmware แยกข้อมูล Valid/Invalid ได้ | มีแล้ว — ตรวจคุณภาพ GGA, ดาวเทียม, HDOP, ความสดและความต่อเนื่องของ Fix | ☐ |
| 12 | GNSS/GPS | GNSS Timeout | ทดสอบในพื้นที่รับสัญญาณไม่ได้ | ระบบยกเลิกการรอเมื่อครบเวลาที่กำหนด | มีแล้ว — ไม่มีพิกัดเดิมรอได้ 120 วินาที มีพิกัดเดิมรอ 30 วินาที | ☐ |
| 13 | GNSS/GPS | Last Known Position | ทำให้รอบถัดไป GNSS Fix ไม่สำเร็จ | ระบบสามารถใช้พิกัดล่าสุดตามเงื่อนไข | มีข้อจำกัด — ใช้ RTC ข้าม Deep Sleep ไม่รับประกันหลังไฟดับ; ไม่เคย Fix อาจเป็น 0,0 | ☐ |
| 14 | GNSS/GPS | ปิด GNSS | จบรอบการรับตำแหน่ง | GNSS ถูกปิดเพื่อลดการใช้พลังงาน | มีแล้ว — หยุด UART/ปิด VEXT ก่อนหลับ ตรวจ VCC จริง ไม่ได้ตัดไฟ Backup ทุกจุด | ☐ |
| 15 | LoRaWAN | OTAA Join | เปิดอุปกรณ์และตรวจ ChirpStack | Device Join Network สำเร็จ | มีแล้ว — ตรวจ Join Request/Accept และค่าประจำอุปกรณ์ตรงกับ Server | ☐ |
| 16 | LoRaWAN | Session Restore | Wake จาก Deep Sleep | สามารถกลับมาใช้ Session เดิมตาม Firmware Design | มีแล้ว — ต้องตรวจหลายรอบหลังแก้ SPI ว่า Session/Frame Counter ต่อเนื่อง | ☐ |
| 17 | LoRaWAN | Normal Uplink | ส่งข้อมูลปกติ | ChirpStack ได้รับ Payload | มีแล้ว — ตรวจ Event Up จริง ไม่ใช้ผล sendReceive แทนหลักฐานรับจาก Server | ☐ |
| 18 | LoRaWAN | Backend Receive | ตรวจ Backend/API | ข้อมูลจาก ChirpStack ถูกส่งถึง Backend | ภายนอก — ตรวจ Integration/API จนถึง Backend ไม่ใช่แค่หน้า ChirpStack | ☐ |
| 19 | LoRaWAN | Payload Decode | ตรวจข้อมูลฝั่ง Server | Latitude, Longitude, Battery, Status ถูก Decode ถูกต้อง | มีแล้ว — มี Decoder ใน README ต้องตั้งที่ Server และตรวจ Payload 15 ไบต์ รวม low_battery ใหม่ | ☐ |
| 20 | LoRaWAN | Failed Transmission | ปิด Gateway หรือทำให้สัญญาณขาด | Firmware จัดการ Retry/Backoff ตาม Logic | มีแล้ว — SOS/Fall ใช้ confirmed หากไม่มี ACK เก็บ Pending แล้วหลับ Retry 60 วินาทีหรือเวลารอ LoRaWAN ที่นานกว่า; Normal ยังเป็น unconfirmed จึงไม่ยืนยันการรับ | ☐ |
| 21 | LoRaWAN | LoRaWAN RX Window | ส่ง Downlink หลัง Uplink | Device สามารถเปิด RX1/RX2 และรับ Downlink ได้ | มีแล้ว — ตรวจ Downlink และ Log ที่ Device; Gateway TX ACK ไม่ยืนยันว่า Device รับแล้ว | ☐ |
| 22 | SOS | ตรวจจับคำสั่ง SOS | กดปุ่มตามรูปแบบที่กำหนด | Firmware ตรวจพบ SOS | มีแล้ว — กดสามครั้ง ตรวจทั้งขณะตื่นและหลับ | ☐ |
| 23 | SOS | SOS Event Priority | เกิด SOS ขณะทำงานปกติ | SOS ได้รับลำดับความสำคัญเหนือ Telemetry ปกติ | มีแล้ว — เมื่อมี Pending SOS เลือกส่ง SOS แทน Normal; หากเกิดระหว่างรอ GNSS จะหยุดรอและใช้พิกัด RTC สร้างแพ็กเก็ต SOS แทน Normal ก่อนส่ง ข้อนี้ไม่ใช่การจัดลำดับ SOS/Fall พร้อมกัน | ☐ |
| 24 | SOS | SOS Payload | ตรวจ Payload ที่ Server | Event ถูกระบุเป็น SOS | มีแล้ว — status=1 ตรวจ Decoder ไม่แสดงเป็น Normal Update | ☐ |
| 25 | SOS | SOS Location | กด SOS แล้วตรวจพิกัด | Payload มีตำแหน่งล่าสุดตาม Logic | มีแล้ว — ใช้พิกัดล่าสุดใน RTC โดยไม่รอ GNSS Fix; ไม่เคย Fix ส่ง 0,0 และ gps_fresh=false ไม่มีการส่งพิกัดตามหลัง SOS โดยเฉพาะ รอบ Normal ถัดไปจึงค้นหาใหม่; การแสดงไม่มีพิกัดเป็นหน้าที่ Application | ☐ |
| 26 | SOS | SOS Delivery | ตรวจ Web Platform | Platform แสดง Alert/Incident | ภายนอก — ตรวจ Alert จริง ถูกอุปกรณ์ ถูก Event และจัดการ Event ซ้ำ | ☐ |
| 27 | SOS | Pending SOS | ทำให้ส่ง SOS ไม่สำเร็จ | SOS ไม่ถูกล้างจนกว่าจะส่งสำเร็จตาม Firmware | มีแล้ว — ใช้ confirmed ล้าง Pending เฉพาะเมื่อได้ LoRaWAN ACK ไม่ใช่แค่รับ Downlink; เก็บ RTC ข้าม Deep Sleep ไม่เพิ่ม NVS ยอมรับว่าสูญหายเมื่อไฟเลี้ยงขาด ACK ไม่ยืนยัน Backend | ☐ |
| 28 | Accelerometer | Sensor Detection | อ่าน Device ID/Register | MCU ติดต่อ Accelerometer ได้ | มีแล้ว — ตรวจ ADXL362 ID และ Readback Configuration | ☐ |
| 29 | Accelerometer | อ่านค่าความเร่ง | เคลื่อนย้ายอุปกรณ์ | อ่านค่า X/Y/Z ได้ | มีแล้ว — ตรวจ Debug และทิศแกนตามการติดตั้งจริง | ☐ |
| 30 | Accelerometer | Interrupt | ทำให้เกิด Event ตาม Threshold | INT สามารถกระตุ้น MCU ได้ | มีแล้ว — INT1 GPIO7 ตรวจการอ่าน/ล้าง Interrupt latch | ☐ |
| 31 | Accelerometer | Wake from Sleep | ทำ Event ขณะ MCU Deep Sleep | MCU ตื่นจาก Accelerometer Interrupt | มีแล้ว — ต้องทดสอบซ้ำหลังแก้ LoRa SPI แม้ Sensor ใช้คนละบัส | ☐ |
| 32 | Accelerometer | Suspected Fall | จำลอง Free-fall/เหตุที่กำหนด | Firmware สร้าง Suspected Fall Event | มีข้อจำกัด — ตรวจ Free-fall ไม่ยืนยันการล้มมนุษย์ทุกแบบ ใช้ Fixture รองรับแรงกระแทก ไม่ให้คนล้มจริง | ☐ |
| 33 | Accelerometer | Pending Fall | ทำให้ส่ง Event ไม่สำเร็จ | Event ถูกเก็บไว้ส่งใหม่รอบถัดไป | มีแล้ว — ใช้ confirmed ล้าง Pending เฉพาะเมื่อได้ LoRaWAN ACK; ไม่มี ACK หลับแล้ว Retry เก็บ RTC ไม่เพิ่ม NVS ยอมรับว่าสูญหายเมื่อไฟเลี้ยงขาด ACK ไม่ยืนยัน Backend | ☐ |
| 34 | Motion / Stationary | Movement Detection | เคลื่อนไหวอุปกรณ์ | ระบบสามารถระบุสถานะการเคลื่อนไหวได้ | ยังไม่มี — ยืนยันความจำเป็น แล้วกำหนดเงื่อนไข/สถานะ Movement แยกจาก Free-fall | ☐ |
| 35 | Motion / Stationary | Stationary Detection* | วางอุปกรณ์นิ่งตามเวลาที่กำหนด | สร้างสถานะ Inactivity/Stationary ตาม Logic | ยังไม่มี — ยืนยันความจำเป็น แล้วกำหนดเวลานิ่งและ Event | ☐ |
| 36 | User Feedback | Status LED | ทดสอบแต่ละสถานะ | LED ทำงานตามสถานะที่กำหนด | มีแล้ว — LED2 ติดตอนทำงาน ดับตอนหลับ กระพริบ OTA แยกจาก LED Charger/GNSS PPS | ☐ |
| 37 | User Feedback | Buzzer | Trigger Event | Buzzer ส่งเสียงตาม Pattern | มีแล้ว — ตรวจแต่ละ Pattern และหยุดเสียงก่อนหลับ | ☐ |
| 38 | User Feedback | SOS Feedback | กด SOS | มี Feedback ยืนยันให้ผู้ใช้งาน | มีแล้ว — เสียงยืนยันการตรวจพบคำสั่ง ไม่ยืนยันการส่งถึง Server | ☐ |
| 39 | User Feedback | Fall Feedback | จำลอง Fall Event | มี Feedback ตาม Firmware Design | มีแล้ว — ตรวจเสียงเมื่อเกิด Suspected Fall | ☐ |
| 40 | User Feedback | Vibration Motor* | Trigger Output | มอเตอร์สั่นทำงานตาม Pattern หาก Firmware รองรับ | ยังไม่มี — มีประกาศขาแต่ไม่มี Pattern ต้องยืนยันความจำเป็น | ☐ |
| 41 | Power Saving | Deep Sleep | จบรอบการทำงาน | MCU เข้าสู่ Deep Sleep | มีแล้ว — ผู้ใช้วัดประมาณ 90 µA หลังปล่อย SPI LoRa ยังต้องทดสอบครบหลายรอบ | ☐ |
| 42 | Power Saving | Timer Wake-up | รอจนถึงรอบถัดไป | MCU ตื่นตาม Timer | มีแล้ว — ตรวจเวลาตาม Mode/ข้อจำกัดเวลาส่ง LoRa เวลาหลับไม่ใช่ระยะระหว่าง Uplink เสมอ | ☐ |
| 43 | Power Saving | SOS Wake-up | กดปุ่มระหว่าง Sleep | MCU ตื่นและประมวลผลเหตุการณ์ | มีแล้ว — GPIO0 ปลุกแล้วตรวจรูปแบบกด ต้องทดสอบ SOS และกลับหลับหลังแก้ SPI | ☐ |
| 44 | Power Saving | Accelerometer Wake-up | Trigger Sensor ขณะ Sleep | MCU ตื่นจาก Sensor Interrupt | มีแล้ว — เช่นข้อ 31 ตรวจ Payload และกลับหลับด้วย | ☐ |
| 45 | Power Saving | Peripheral Power Control | ตรวจ GNSS/Peripheral | ปิด Peripheral ที่ไม่จำเป็นก่อน Sleep | มีแล้ว — LoRa sleep/ปล่อย SPI, VEXT OFF, LED/Buzzer OFF; ADXL362 ทำงานต่อเพื่อปลุก Fall | ☐ |
| 46 | Power Saving | Wi-Fi/Bluetooth Off | Normal Operation | ไม่เปิดระบบที่ไม่จำเป็นในช่วง Tracking | มีแล้ว — Wi-Fi ใช้ใน OTA ตรวจปิดหลังออก OTA และกลับ Tracking | ☐ |
| 47 | Low Power Mode | เปิด Low Power Mode | ส่ง Downlink Command | Device เปลี่ยนเป็นรอบ Low Power | มีแล้ว — FPort 10, AQAP; ปัจจุบันหลับ 15 นาที แยกจาก Low Battery | ☐ |
| 48 | Low Power Mode | กลับ Normal Mode | ส่ง Downlink Command | Device กลับสู่รอบ Normal | มีแล้ว — FPort 10, AAAA; ปัจจุบันหลับ 1 นาที ตรวจ Log/เวลาจริง | ☐ |
| 49 | Low Power Mode | Persistence | เข้า Deep Sleep แล้ว Wake | สถานะ Power Mode ยังคงถูกต้อง | มีข้อจำกัด — เก็บ RTC ข้าม Deep Sleep ไม่รับประกันหลังไฟดับ ต้องยืนยันความต้องการเก็บถาวร | ☐ |
| 50 | Telemetry | Device ID | ตรวจ Payload | ระบุอุปกรณ์ได้ถูกต้อง | มีข้อจำกัด — DevEUI อยู่ใน ChirpStack Event Envelope ไม่ซ้ำใน Payload 15 ไบต์ Backend ต้องใช้ Envelope ระบุอุปกรณ์ | ☐ |
| 51 | Telemetry | Position | ตรวจ Payload | Latitude/Longitude ถูกต้อง | มีข้อจำกัด — Float32 จาก Fix ผ่านเกณฑ์หรือ RTC ต้องดู gps_fresh; ไม่มี Fix อาจเป็น 0,0 | ☐ |
| 52 | Telemetry | Battery | ตรวจ Payload | Battery Status ถูกส่ง | มีแล้ว — mV/เปอร์เซ็นต์/battery_valid และ low_battery ใหม่; ADC invalid ให้ Decoder แสดง null | ☐ |
| 53 | Telemetry | Event Status | Normal/SOS/Fall | Event Code ถูกต้อง | มีแล้ว — 0=Normal, 1=SOS, 2=Suspected Fall ตรวจ Decoder จริง | ☐ |
| 54 | Telemetry | Firmware Version | ตรวจ Payload/Status | ระบุ Firmware Version ได้ | มีแล้ว — Major/Minor 2 ไบต์ใน Uplink ปัจจุบัน 1.3 ไม่ใช่ Custom Field ใน DevStatusAns | ☐ |
| 55 | Telemetry | Timestamp | ตรวจ Server Record | บันทึกเวลาของข้อมูลได้ถูกต้องตามการออกแบบ | ภายนอก — ใช้เวลา ChirpStack/Server ไม่มีเวลา GNSS ใน Payload จึงไม่ใช่เวลาวัดพิกัดเก่า | ☐ |
| 56 | OTA | เข้า OTA Mode | ใช้คำสั่ง/วิธีเข้าสู่ OTA | Device เปิด OTA Mode | มีแล้ว — ทดสอบรูปแบบปุ่มไม่ชนกับ SOS/ปิดเครื่อง | ☐ |
| 57 | OTA | Wi-Fi AP | ตรวจ SSID | พบ Access Point ของ Tracker | มีแล้ว — ตรวจ SSID/การเชื่อมตาม ota_config.h ไม่บันทึกรหัสผ่านลงหลักฐาน | ☐ |
| 58 | OTA | Web OTA | เปิดหน้า OTA | สามารถเข้าหน้า Upload Firmware | มีแล้ว — ตรวจหน้า Web หลังเชื่อม AP | ☐ |
| 59 | OTA | Firmware Upload | Upload Firmware | เขียน Firmware ใหม่สำเร็จ | มีแล้ว — Arduino Update ตรวจไฟล์ที่เหมาะสมและผล Upload จริง | ☐ |
| 60 | OTA | Restart หลัง OTA | จบ Update | Device Restart และทำงานด้วย Firmware ใหม่ | มีแล้ว — ตรวจเวอร์ชันและรอบ Tracking หลัง Restart | ☐ |
| 61 | OTA | OTA Timeout | ไม่ใช้งานตามเวลาที่กำหนด | Wi-Fi ปิดและกลับสู่ Low Power Operation | มีแล้ว — Idle 15 นาที ปิด AP แล้ว Timed Sleep ตรวจกลับ Tracking หลังตื่น | ☐ |
| 62 | Recovery | GNSS Failure | ปิดบังสัญญาณดาวเทียม | ระบบไม่ค้างและกลับสู่รอบการทำงานได้ | มีแล้ว — ตรวจ Timeout/Cached Position และไม่มี gps_fresh เมื่อไม่ผ่านเกณฑ์ | ☐ |
| 63 | Recovery | LoRaWAN Join Failure | ปิด Gateway | ระบบ Timeout/Retry โดยไม่ค้าง | มีแล้ว — ตรวจ Join Timeout/Retry หลังหลับ ไม่สับสนกับ Unconfirmed TX | ☐ |
| 64 | Recovery | Sensor Failure | ตัด/จำลอง Sensor Error | ระบบจัดการ Error โดยไม่ทำให้ MCU ค้าง | มีข้อจำกัด — ตรวจ ID/Readback และปิด Fall Wake เมื่อ Init ไม่ผ่าน รอทดสอบ Fault จริงรวมเสียระหว่างทำงาน | ☐ |
| 65 | Recovery | Watchdog / Unexpected Reset | จำลอง Reset | ระบบสามารถกลับมาทำงานได้ | มีข้อจำกัด — มีเส้นทาง Boot แต่ยังไม่ยืนยันทุก Fault ต้องตรวจ Nonce/Session/Pending Event และข้อมูล RTC ที่สูญหาย | ☐ |

## งาน Implement ต่อเนื่องจาก Checklist

อ้างอิงเลขแถวด้านบนเพื่อเทียบกับ Sheets โดยตรง รายการไม่จำเป็นต้องทำทั้งหมด ให้ตัดสินใจเรื่องขอบเขตก่อน

- [x] **ข้อ 06 — Low Battery ฝั่ง Firmware:** ทำแล้วใน `4fbc48d` พร้อม Host Test และ Decoder ใน README เก็บ RTC ไม่เขียน NVS ไม่เปลี่ยนรอบส่ง ไม่ส่งเสียงหรือปิดเครื่องอัตโนมัติ
- [ ] **ข้อ 06, 19, 52 — Server/Application:** อัปเดต Decoder ให้เห็น `flags.low_battery` และแจ้งเตือนเฉพาะตอนเข้าสถานะ Low/พบ Low ครั้งแรก ไม่แจ้งซ้ำทุก Uplink และไม่ตีความ null ว่าหายแบตต่ำ
- [ ] **ข้อ 06 — ทดสอบจริง:** ตรวจ Threshold ขาลง/ขาขึ้นและข้าม Deep Sleep ใช้แหล่งจ่ายปลอดภัยตามวงจร ไม่คายประจุแบตต่ำเกินข้อกำหนด
- [x] **ข้อ 20, 27, 33 — ยืนยันการส่งฝั่ง Firmware:** เลือก confirmed เฉพาะ SOS/Fall; Normal คง unconfirmed ล้าง Pending เมื่อได้ ACK เท่านั้น หากไม่มี ACK หลับ Retry 60 วินาทีหรือเวลารอ LoRaWAN ที่นานกว่า ไม่เพิ่ม Application ACK/NVS ยอมรับพลังงาน/Downlink/ข้อมูลซ้ำที่เพิ่มขึ้น ยังต้องทดสอบรับ ACK และกรณีปิด Gateway บนเครื่องจริง; LoRaWAN ACK ไม่ยืนยันว่า Web Platform สร้าง Alert สำเร็จ
- [x] **ข้อ 27, 33 — Pending หลังไฟดับ:** ตกลงใช้ RTC ตามเดิม ไม่เพิ่ม NVS สำหรับ Pending SOS/Fall ยังคงเก็บข้าม Deep Sleep และยอมรับว่าเหตุที่ยังส่งไม่สำเร็จอาจหายเมื่อแบตหลุด แบตหมด หรือไฟเลี้ยงขาด การตัดสินใจนี้ไม่ใช่ผลผ่านการทดสอบบนเครื่องจริง และไม่เปลี่ยนเงื่อนไขยืนยันการส่งในข้อก่อนหน้า
- [ ] **ข้อ 13, 49 — Persistence หลังไฟดับ:** ยืนยันว่าพิกัดเก่า/Low Power ต้องคงหลังถอดแบตหรือไม่
- [x] **ข้อ 23 — SOS เหนือ Normal:** Firmware รองรับแล้ว ทั้งการเลือก SOS แทน Normal และการหยุดรอ GNSS เมื่อมี SOS เพื่อใช้พิกัด RTC ส่งเหตุแทน ไม่ต้องเพิ่มโค้ดสำหรับเงื่อนไขนี้; ยังต้องทดสอบบนเครื่องจริงตามแถว 23 ก่อนติ๊กผ่านใน Sheets
- [x] **ข้อ 25 — SOS ไม่รอพิกัด:** Firmware ใช้พิกัดล่าสุดใน RTC ทันที ไม่เคย Fix ส่ง 0,0 และ gps_fresh=false จึงไม่ใช่งาน Implement ที่ขาดอยู่ ส่วนการส่งพิกัดตามหลัง SOS โดยเฉพาะยังไม่มีและเป็นเพียงทางเลือกเพิ่มเติม ไม่ใช่พฤติกรรมปัจจุบัน; ฝั่ง Application ยังต้องทดสอบว่าไม่แสดง 0,0 เป็นตำแหน่งจริง และช่องผ่านบนเครื่องจริงยังไม่ถูกติ๊ก
- [ ] **ข้อ 34 — Movement:** ยืนยันความจำเป็นก่อนออกแบบ/Implement
- [ ] **ข้อ 35 — Stationary:** ยืนยันความจำเป็นก่อนกำหนดเวลานิ่ง/Implement
- [ ] **ข้อ 40 — Vibration:** ยืนยันความจำเป็นและ Pattern ก่อน Implement
- [ ] **ข้อ 16, 31, 41–45 — Regression หลังลดกระแส:** ทดสอบ Timer/SOS/Fall หลายรอบ ตรวจการส่งและ Counter ผล 90 µA ไม่ยืนยันว่าทุกฟังก์ชันผ่าน

## การบันทึกผลใน Sheets

ติ๊กผ่านเฉพาะแถวที่ทดสอบตรงผลที่คาดหวังแล้ว หากยอมรับข้อจำกัด ให้บันทึกเงื่อนไขการยอมรับ เพิ่มคอลัมน์ Firmware Version/Commit, วันที่, ผู้ทดสอบ และหลักฐานตามต้องการ รายการไม่ใช้ระบุ N/A พร้อมเหตุผล ไม่ติ๊กว่าผ่าน และไม่ใส่ข้อมูลลับลงหลักฐาน
