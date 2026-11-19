(() => {
  // =========================================================
  // KONFIGURASI SUPABASE (ISI SESUAI PROJECT SUPABASE KAMU)
  // =========================================================
  const SUPABASE_URL = 'https://ejfftyrdmcrdkqvxxhxy.supabase.co';
  const SUPABASE_ANON_KEY = 'sb_publishable_Zf7P8fH6ULcn2ll90UVMBA_IG7umqkn';
  const TABLE_NAME = 'mesin_briket';
  const ROW_ID = 1;
  const ESP_TIMEOUT_MS = 8500;

  let sb = null;
  let isEmergency = false;
  let lastKnownState = 'STOPPED';
  let prevCount = null;
  let sessionCount = 0;
  let lastEspHeartbeat = 0;
  const cutTimes = [];

  const $ = (id) => document.getElementById(id);

  function nowClock() {
    return new Date().toTimeString().slice(0, 8);
  }

  function updateClock() {
    const el = $('liveClock');
    if (el) el.textContent = nowClock();
  }
  updateClock();
  setInterval(updateClock, 1000);

  function setConn(state, text) {
    $('connPill').className = `link-badge ${state}`;
    $('connText').textContent = text;
  }

  function refreshMetrics() {
    const now = Date.now();
    while (cutTimes.length && now - cutTimes[0] > 60000) {
      cutTimes.shift();
    }
    $('rateChip').textContent = `${cutTimes.length} / MNT`;
    $('sessionText').textContent = `+${sessionCount}`;

    if (lastEspHeartbeat > 0 && now - lastEspHeartbeat > ESP_TIMEOUT_MS) {
      setConn('connecting', 'ESP32 IDLE');
    }
  }

  setInterval(refreshMetrics, 2500);

  function renderCounterDigits(val) {
    const clean = Math.max(0, Math.floor(val));
    const str = String(clean);
    const padLen = Math.max(0, 4 - str.length);
    $('ghostDigits').textContent = '0'.repeat(padLen);
    $('countNum').textContent = clean.toLocaleString('id-ID');
  }

  function triggerCounterPulse() {
    const frame = $('counterFrame');
    frame.classList.remove('pulse');
    void frame.offsetWidth;
    frame.classList.add('pulse');
    setTimeout(() => frame.classList.remove('pulse'), 420);
  }

  function renderState(data) {
    if (!data) return;

    const st = String(data.state || 'STOPPED').toUpperCase();
    const count = Number.isFinite(Number(data.count)) ? Number(data.count) : 0;
    const ch1 = Boolean(data.ch1_wiper);
    const ch2 = Boolean(data.ch2_ac);
    const emerg = Boolean(data.emergency) || st === 'EMERGENCY';

    if (data.updated_at) {
      const ts = new Date(data.updated_at).getTime();
      if (!Number.isNaN(ts)) {
        lastEspHeartbeat = ts;
        if (Date.now() - ts <= ESP_TIMEOUT_MS) {
          setConn('online', 'ONLINE');
        }
      }
    }

    if (prevCount !== null && count > prevCount) {
      const diff = count - prevCount;
      sessionCount += diff;
      for (let i = 0; i < diff; i++) cutTimes.push(Date.now());
      triggerCounterPulse();
    }
    prevCount = count;
    isEmergency = emerg;

    renderCounterDigits(count);
    $('syncLabel').textContent = nowClock();
    $('emergBanner').classList.toggle('hidden', !emerg);

    const screen = $('oledScreen');
    const dot = $('statusDot');
    screen.className = 'oled-display';
    dot.className = 'phosphor-dot';

    const isActiveRun = st === 'RUNNING' || st === 'CUTTING';
    $('ledStart').classList.toggle('active', isActiveRun && !emerg);
    $('ledStop').classList.toggle('active', !isActiveRun || emerg);

    $('btnStart').classList.toggle('latched', isActiveRun && !emerg);
    $('btnStop').classList.toggle('latched', !isActiveRun && !emerg);

    const stepExtrude = $('stepExtrude');
    const stepDetect = $('stepDetect');
    const stepCut = $('stepCut');
    stepExtrude.className = 'cycle-node';
    stepDetect.className = 'cycle-node';
    stepCut.className = 'cycle-node';

    const cutWrap = $('cutProgressWrap');
    const cutBar = $('cutProgressBar');

    if (emerg) {
      screen.classList.add('emergency');
      dot.classList.add('emergency');
      $('modeTag').textContent = 'DARURAT';
      $('statusTitle').textContent = 'Interlock Darurat Aktif';
      $('statusDesc').textContent = 'Mesin terkunci darurat. Lepas saklar push-lock fisik untuk melanjutkan.';
      cutWrap.classList.add('hidden');
      cutBar.classList.remove('animating');
    } else if (st === 'CUTTING') {
      screen.classList.add('cutting');
      dot.classList.add('cutting');
      $('modeTag').textContent = 'MEMOTONG';
      $('statusTitle').textContent = 'Sedang Memotong Briket';
      $('statusDesc').textContent = 'Motor AC jeda 3 detik sementara wiper memutar pisau pemotong.';
      stepDetect.classList.add('on-amber');
      stepCut.classList.add('on-amber');
      cutWrap.classList.remove('hidden');
      if (lastKnownState !== 'CUTTING') {
        cutBar.classList.remove('animating');
        void cutBar.offsetWidth;
        cutBar.classList.add('animating');
      }
    } else if (st === 'RUNNING') {
      screen.classList.add('running');
      dot.classList.add('running');
      $('modeTag').textContent = 'BERJALAN';
      $('statusTitle').textContent = 'Mesin Berjalan';
      $('statusDesc').textContent = 'Motor AC aktif mendorong adonan briket menuju sensor proximity.';
      stepExtrude.classList.add('on-green');
      stepDetect.classList.add('on-green');
      cutWrap.classList.add('hidden');
      cutBar.classList.remove('animating');
    } else {
      screen.classList.add('stopped');
      dot.classList.add('stopped');
      $('modeTag').textContent = 'BERHENTI';
      $('statusTitle').textContent = 'Mesin Berhenti';
      $('statusDesc').textContent = 'Tekan START untuk menjalankan motor cetak dan pemotongan otomatis.';
      cutWrap.classList.add('hidden');
      cutBar.classList.remove('animating');
    }

    lastKnownState = st;

    $('btnStart').disabled = emerg || st === 'RUNNING' || st === 'CUTTING';
    $('btnStop').disabled = emerg || st === 'STOPPED';

    const acCls = ch2 ? 'on' : (st === 'CUTTING' ? 'cut' : 'off');
    $('acBadge').className = `ann-state mono ${acCls}`;
    $('acBadge').textContent = ch2 ? 'MENYALA' : (st === 'CUTTING' ? 'JEDA 3D' : 'MATI');
    $('cardAc').className = `ann-cell ${ch2 ? 'on-green' : (st === 'CUTTING' ? 'on-amber' : '')}`;

    $('wiperBadge').className = `ann-state mono ${ch1 ? 'cut' : 'off'}`;
    $('wiperBadge').textContent = ch1 ? 'POTONG' : 'SIAP';
    $('cardWiper').className = `ann-cell ${ch1 ? 'on-amber' : ''}`;

    $('emergBadge').className = `ann-state mono ${emerg ? 'danger' : 'ok'}`;
    $('emergBadge').textContent = emerg ? 'KUNCI' : 'AMAN';
    $('cardEmerg').className = `ann-cell ${emerg ? 'on-red' : ''}`;

    refreshMetrics();
  }

  async function sendCommand(cmd) {
    if (!sb) {
      alert('Koneksi Supabase belum siap. Periksa URL/Key atau koneksi internet.');
      return;
    }
    if (isEmergency && cmd !== 'RESET_CNT') return;

    // Umpan balik visual langsung di layar saat tombol disentuh
    if (cmd === 'START') {
      $('statusTitle').textContent = 'Mengirim Perintah START...';
      $('btnStart').classList.add('latched');
    } else if (cmd === 'STOP') {
      $('statusTitle').textContent = 'Mengirim Perintah STOP...';
      $('btnStop').classList.add('latched');
    }

    const patch = { cmd };
    if (cmd === 'RESET_CNT') {
      patch.count = 0;
      sessionCount = 0;
      prevCount = 0;
      renderCounterDigits(0);
      refreshMetrics();
    }

    const { error } = await sb
      .from(TABLE_NAME)
      .update(patch)
      .eq('id', ROW_ID);

    if (error) {
      console.error('Gagal kirim perintah ke Supabase:', error.message);
      setConn('offline', 'ERR CMD');
      $('statusTitle').textContent = 'Gagal Kirim Perintah';
      $('statusDesc').textContent = error.message;
    }
  }

  async function fetchInitialState() {
    if (!sb) return;
    const { data, error } = await sb
      .from(TABLE_NAME)
      .select('*')
      .eq('id', ROW_ID)
      .single();

    if (error) {
      console.error('Supabase fetch error:', error.message);
      setConn('offline', 'DB ERROR');
      return;
    }
    renderState(data);
  }

  function initSupabase() {
    if (SUPABASE_URL.includes('YOUR-PROJECT-ID') || !window.supabase) {
      setConn('connecting', 'SET KEY');
      $('statusDesc').textContent = 'Isi SUPABASE_URL dan SUPABASE_ANON_KEY di app.js untuk terhubung ke ESP32.';
      return;
    }

    setConn('connecting', 'SYNCING');
    sb = window.supabase.createClient(SUPABASE_URL, SUPABASE_ANON_KEY);

    fetchInitialState();

    sb.channel('briket-realtime')
      .on(
        'postgres_changes',
        {
          event: 'UPDATE',
          schema: 'public',
          table: TABLE_NAME,
          filter: `id=eq.${ROW_ID}`
        },
        (payload) => {
          renderState(payload.new);
        }
      )
      .subscribe((status) => {
        if (status === 'SUBSCRIBED') {
          setConn('online', 'ONLINE');
        } else if (status === 'CHANNEL_ERROR' || status === 'TIMED_OUT') {
          setConn('offline', 'OFFLINE');
        }
      });

    // Polling cadangan tiap 3 detik untuk sinkronisasi & cek detak jantung ESP32
    setInterval(fetchInitialState, 3000);
  }

  $('btnStart').addEventListener('click', () => {
    if (lastKnownState !== 'RUNNING' && lastKnownState !== 'CUTTING') {
      sendCommand('START');
    }
  });

  $('btnStop').addEventListener('click', () => {
    if (lastKnownState !== 'STOPPED') {
      sendCommand('STOP');
    }
  });

  $('btnReset').addEventListener('click', () => {
    if (confirm('Reset hitungan briket menjadi 0?')) {
      sendCommand('RESET_CNT');
    }
  });

  renderState({ state: 'STOPPED', count: 0, ch1_wiper: false, ch2_ac: false, emergency: false });
  initSupabase();
})();


