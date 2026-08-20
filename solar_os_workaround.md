# Solar OS — Sistem Mimarisi ve Çözüm Rehberi (Workarounds & Best Practices)

Bu doküman, Solar OS üzerinde geliştirilen tüm uygulamalar için geçerli olan kritik mimari kuralları, performans optimizasyonlarını ve sistem genelinde uygulanması gereken en iyi pratikleri içermektedir.

---

## 1. İki Katmanlı FPS ve Bağımsız Kürsör Mekanizması (Dual-Rate Compositor)

### 📌 Problem Tanımı
Waveshare 4.2" RLCD (ST7305) gibi yansıtıcı veya E-Ink tipi ekranlarda tam ekran yenileme (`solar_os_gfx_clear` + `solar_os_gfx_present`) panel üzerinde gözle görülür bir beyaz/siyah yanıp sönme (flicker) ve kablo temassızlığı hissi yaratır. Ancak fare imlecinin (BLE / USB Mouse) kullanıcıya akıcı hissettirebilmesi için en az **30 FPS (33ms)** yenileme hızına ihtiyacı vardır. 

Eğer fare hareketi uygulamanın kendi ekran çizim döngüsüne bağlanırsa:
- Uygulama yavaş yenilendiğinde fare takılarak hareket eder.
- Fareyi akıcı yapmak için uygulamanın her karede tam ekran çizmesi sağlanırsa ekran sürekli titrer/flicker yapar.

### ⚙️ Çözüm Mimarisi
Sistemde **Uygulama Katmanı** ile **Kürsör Kompozitör Katmanı** tamamen birbirinden ayrılmıştır:

```
+-------------------------------------------------------------------------+
|                              Solar OS Ekranı                            |
|                                                                         |
|   +-----------------------------------------------------------------+   |
|   | Uygulama Katmanı (App Layer): 0.2 - 1 FPS / Olay Tabanlı         |   |
|   | - Sadece veri değiştiğinde veya kullanıcı etkileşiminde çizer.  |   |
|   | - Statik widget'lar, tablolar, metinler, grafikler.            |   |
|   +-----------------------------------------------------------------+   |
|                                    ^                                    |
|                                    | (Bağımsız Tampon)                  |
|   +-----------------------------------------------------------------+   |
|   | Kürsör Katmanı (Compositor Layer): 30 FPS Sabit (33ms)          |   |
|   | - `dispatch_mouse_compositor()` tarafından yürütülür.           |   |
|   | - 16x16 piksellik bölgeyi kurtarır (`save/restore compositor`). |   |
|   | - Uygulama render'ını tetiklemeden sadece imleci günceller.     |   |
|   +-----------------------------------------------------------------+   |
+-------------------------------------------------------------------------+
```

### 📋 Uygulama Geliştirme Kuralları
1. **Olay Tabanlı Çizim:** Grafik uygulamaları her `SOLAR_OS_EVENT_TICK` geldiğinde tam ekran çizim yapmamalıdır. Sadece kullanıcı eylemlerinde (`SOLAR_OS_EVENT_CHAR`, `SOLAR_OS_EVENT_CLICK`, `SOLAR_OS_EVENT_SCROLL`, `SOLAR_OS_EVENT_RESUME`) veya uzun aralıklı periyotlarla (ör. 5 saniye) çizim yapılmalıdır.
2. **Kürsör Kilidi Kontrolü:** `main.c` içerisindeki `dispatch_mouse_compositor` fonksiyonu, oturum grafik durumuna takılmadan ön planda grafik uygulaması veya diyalog olduğu sürece 30 FPS hızında bağımsız çalışmalıdır:
   ```c
   static void dispatch_mouse_compositor(void)
   {
       static uint32_t last_track_ms = 0;
       if (display_u8g2 == NULL ||
           solar_os_sessions_foreground_is_shell() ||
           !solar_os_mouse_is_dirty()) {
           return;
       }
       ...
       solar_os_mouse_compositor_track_tick(display_u8g2);
       solar_os_mouse_clear_dirty();
   }
   ```
3. **Terminal Çizim Güvenliği:** `draw_terminal_if_needed()` kesinlikle sadece ön planda gerçek Shell oturumu (`solar_os_sessions_foreground_is_shell()`) varken çalışmalı, hiçbir grafik uygulamasının üzerine terminal tamponu basmamalıdır.

---

## 2. Bellek Kabulü ve Görev Yöneticisi Başlatma Stratejisi (Task Admission & State Safety)

### 📌 Problem Tanımı
Arka planda müzik çalar (`player`), web radyosu veya ses akışı gibi FreeRTOS worker görevi ve DMA tamponu kullanan uygulamalar çalışırken, dahili SRAM'in bir kısmı tahsis edilir. Yeni bir uygulama başlatılmak istendiğinde sistem `solar_os_task_admit` bellek kabul denetimi yapar.

Eğer bir UI uygulaması `.worker_stack_bytes` alanına gereksiz bir yığın boyutu (örn. 8192 B) yazarsa, sistem `SOLAR_OS_INTERNAL_LAUNCH_RESERVE_BYTES` (32 KB güvenlik marjı) ile birlikte bu boşluğu bulamaz ve uygulamayı **"admission denied"** diyerek açılmadan kapatır.

Ayrıca FreeRTOS'un `uxTaskGetSystemState()` fonksiyonuna PSRAM (harici bellek) adresi verilmesi, ESP32-S3'ün zamanlayıcı kilitleri altındayken Önbellek Kilitlenmesi (Cache Panic) yaşamasına neden olur.

### ⚙️ Çözüm Mimarisi

```
+-------------------------------------------------------------------------+
|                      Uygulama Başlatma ve Bellek Mimarisi                |
+-------------------------------------------------------------------------+
| 1. Worker Stack Ayrımı:                                                 |
|    - FreeRTOS Görevi Açmayan UI Uygulamaları: worker_stack_bytes = 0    |
|    - Ayrı Thread Başlatan Servis/Uygulamalar: worker_stack_bytes > 0    |
|                                                                         |
| 2. FreeRTOS & Donanım Sorgu Güvenliği:                                  |
|    - `uxTaskGetSystemState` hedef dizisi: Yerel DRAM / Stack üzerinde   |
|    - PSRAM içine kritik kesme anında yazım YAPILMAZ                     |
|                                                                         |
| 3. Oturumlar Arası İzolasyon:                                           |
|    - Liste sorgularında (`get_all_info`) başka oturumun bağlamı         |
|      (`session_prepare_context`) çalınmaz, başlık tablodan okunur.      |
+-------------------------------------------------------------------------+
```

### 📋 Uygulama Geliştirme Kuralları
1. **`worker_stack_bytes` Değeri:**
   - Eğer uygulamanız ayrı bir `xTaskCreate` / `solar_os_task_create` thread'i **başlatmıyorsa**, `solar_os_app_t` tanımında `.worker_stack_bytes = 0` olmalıdır.
   - Bu sayede arka planda ne kadar ağır ses veya ağ işlemi olursa olsun uygulamanız bellek reddine takılmadan anında açılır.
   ```c
   const solar_os_app_t solar_os_esprocess_app = {
       .name = "esprocess",
       .summary = "task, process, cpu and memory monitor and manager",
       .flags = 0,
       .start = esprocess_start,
       .stop = esprocess_stop,
       .event = esprocess_event,
       .state_slot = &esprocess_state_ptr,
       .state_size = sizeof(esprocess_state_t),
       .state_storage = SOLAR_OS_APP_STATE_EXTERNAL_PREFERRED,
       .worker_stack_bytes = 0, // Event-driven UI olduğu için 0
   };
   ```

2. **DRAM vs PSRAM Yığın Güvenliği:**
   - FreeRTOS çekirdek durumunu (`uxTaskGetSystemState`) sorgularken hedef tamponu PSRAM'de tutulan `epstate` içinde barındırmayın.
   - Fonksiyon içinde yerel yığıtta (DRAM) `TaskStatus_t raw_tasks[ESPROCESS_MAX_TASKS];` tanımlayarak güvenli şekilde snapshot alın.

3. **Oturum Bilgisi Okuma İzolasyonu:**
   - Sistem genelindeki oturum listelerini gezerken (`solar_os_sessions_get_all_info`), arka planda çalışan uygulamanın durumunu veya ses akışını bozmamak için o oturumun bağlamına (`session_prepare_context`) geçiş yapılmamalı, doğrudan oturum kaydı okunmalıdır.

---

## 3. Sistem Geneli Kontrol Listesi (Checklist)

Tüm Solar OS uygulamalarını bu standartlara uyarlarken aşağıdaki adımlar izlenmelidir:

- [ ] **Grafik / Render Frekansı:** Uygulama her tick'te gereksiz `solar_os_gfx_clear` yapıyor mu? (Sadece olaylarda veya uzun periyotlarda yenilenmeli).
- [ ] **Fare İmleci Uyumu:** `main.c` compositor bağımsızlığı sayesinde imleç 30 FPS akıyor mu?
- [ ] **Worker Stack Kontrolü:** Ayrı FreeRTOS thread'i olmayan uygulamalarda `worker_stack_bytes` 0 mı?
- [ ] **Çıkış Diyaloğu Desteği:** `SOLAR_OS_KEY_ESCAPE` ile çıkarken onay modalı ve `Kucult` (minimize) seçeneği düzgün çalışıyor mu?
- [ ] **RLCD Yüksek Kontrast:** Diyaloglar ve pencereler şeffaf font modunda koyu kutular yerine net çerçeve ve siyah metinle mi çiziliyor?
