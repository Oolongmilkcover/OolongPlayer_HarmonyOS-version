import libentry, { PlayerEvent } from 'libentry.so';

/** op_event_type */
export const EVENT_OPENED: number = 1;
export const EVENT_POSITION: number = 2;
export const EVENT_STATE: number = 3;
export const EVENT_SEEK_DONE: number = 4;
export const EVENT_COMPLETED: number = 6;
export const EVENT_ERROR: number = 7;

/** op_state：只关心播放/暂停两类，用于校正 UI */
export const STATE_PLAYING: number = 3;
export const STATE_PAUSED: number = 4;
export const STATE_STOPPED: number = 5;
export const STATE_ENDED: number = 6;

/** 打开成功后带的文件级属性（一次打开期间不变） */
export class MediaInfo {
  durationMs: number = 0;
  width: number = 0;
  height: number = 0;
  hasVideo: boolean = false;
  hasAudio: boolean = false;
  isStream: boolean = false;
  isMusic: boolean = false;
  isAudioMedia: boolean = false;
}

// seek 相关常量与 Qt 壳保持一致
const SEEK_STEP_MS: number = 5000;
const SEEK_DEBOUNCE_MS: number = 120;
const SEEK_UI_HOLD_MS: number = 1500;
const SEEK_LAND_TOLERANCE_MS: number = 1000;
const SEEK_LOCK_TIMEOUT_MS: number = 2000;
const SEEK_ACTIVE_WINDOW_MS: number = 1200;

/**
 * 播放器控制器：包装 libentry 原生接口，并复刻 Qt 壳的 seek 状态机
 * （前沿立即生效 + 尾部去抖、串行化、进度条钉住目标、落点校正）。
 */
export class PlayerController {
  public onOpened: (info: MediaInfo) => void = (info: MediaInfo): void => {};
  public onCompleted: () => void = (): void => {};
  public onError: (message: string) => void = (message: string): void => {};
  public onDisplay: (positionMs: number) => void = (positionMs: number): void => {};
  public onState: (state: number) => void = (state: number): void => {};

  private initialized: boolean = false;
  private streamMode: boolean = false;
  private speed: number = 1.0;

  private positionMs: number = 0;
  private durationMs: number = 0;

  private canSeek: boolean = true;
  private canSeekUntil: number = 0;
  private pendingSeekMs: number = -1;
  private seekTargetMs: number = -1;
  private seekHoldUntil: number = 0;
  private seekActiveUntil: number = 0;
  private dragging: boolean = false;
  private timerId: number = -1;
  // seek 不靠暂停实现，但 op_seek_ms 可能打断播放：记下原状态，SEEK_DONE 后恢复
  private seekChainActive: boolean = false;
  private resumeAfterSeek: boolean = false;

  public init(): void {
    libentry.init((event: PlayerEvent): void => {
      this.handleEvent(event);
    });
  }

  public release(): void {
    this.clearTimer();
    this.initialized = false;
    libentry.destroy();
  }

  /* ------------------------------------------------------------ 表面 */

  public attachSurface(surfaceId: string, width: number, height: number): boolean {
    return libentry.attachSurface(surfaceId, width, height);
  }

  public resizeSurface(width: number, height: number): void {
    libentry.resizeSurface(width, height);
  }

  /** 尺寸变化后主动重建渲染表面并重绘最后一帧（暂停时也能立刻恢复正确比例） */
  public refreshSurface(width: number, height: number): void {
    libentry.refreshSurface(width, height);
  }

  public detachSurface(): void {
    libentry.detachSurface();
  }

  /* ------------------------------------------------------------ 打开 / 控制 */

  public open(path: string, fd: number): number {
    return libentry.open(path, fd, 0, 0);
  }

  public play(): void {
    if (!this.initialized) return;
    libentry.play();
  }

  public pause(): void {
    if (!this.initialized) return;
    // 用户明确暂停：取消 seek 完成后的恢复播放
    this.seekChainActive = false;
    this.resumeAfterSeek = false;
    libentry.pause();
  }

  public stop(): void {
    libentry.stop();
    this.initialized = false;
    this.streamMode = false;
    this.durationMs = 0;
    this.positionMs = 0;
    this.resetSeekState();
    this.onDisplay(0);
  }

  public setFilter(type: number): void {
    libentry.setFilter(type);
  }

  public changeSpeed(delta: number): number {
    if (this.streamMode) return this.speed;
    const base = this.speed > 0 ? this.speed : 1.0;
    let next = base + delta;
    if (next < 0.5) next = 0.5;
    if (next > 2.0) next = 2.0;
    libentry.setSpeed(Math.round(next * 10));
    const reported = libentry.getSpeedX10() / 10;
    this.speed = reported > 0 ? reported : next;
    return this.speed;
  }

  public getSpeed(): number {
    return this.speed;
  }

  /* ------------------------------------------------------------ 快进 / 快退 */

  public fastForward(): void {
    this.queueSeekBy(SEEK_STEP_MS);
  }

  public rewind(): void {
    this.queueSeekBy(-SEEK_STEP_MS);
  }

  private queueSeekBy(deltaMs: number): void {
    if (!this.initialized || this.streamMode) return;

    // 即使这次因为"上一次 seek 还没落地"被丢弃，也要记下用户还在连续操作
    this.seekActiveUntil = Date.now() + SEEK_ACTIVE_WINDOW_MS;
    if (!this.canSeek) return;

    const total = this.durationMs;
    if (total <= 0) return;

    const base: number = this.pendingSeekMs >= 0 ? this.pendingSeekMs
      : (this.seekTargetMs >= 0 ? this.seekTargetMs : this.positionMs);
    let target = base + deltaMs;
    if (target < 0) target = 0;
    if (target > total) target = total;

    this.display(target);

    if (this.timerId < 0) {
      // 前沿：这一串按键的第一次立即生效
      this.pendingSeekMs = -1;
      this.postSeek(target);
    } else {
      // 后续按键：只累积，静默后再 seek 末次
      this.pendingSeekMs = target;
      this.pin(target);
    }

    if (this.timerId >= 0) clearTimeout(this.timerId);
    this.timerId = setTimeout(() => {
      this.timerId = -1;
      const pending = this.pendingSeekMs;
      this.pendingSeekMs = -1;
      if (this.dragging) return;
      if (pending >= 0) {
        if (this.canSeek) {
          this.postSeek(pending);
        } else {
          this.pendingSeekMs = pending;
        }
      }
    }, SEEK_DEBOUNCE_MS);
  }

  private postSeek(targetMs: number): void {
    this.display(targetMs);
    this.pin(targetMs);

    // 直接调 op_seek_ms，不额外 pause；只记住原本是否在播，SEEK_DONE 后据它恢复
    this.resumeAfterSeek = this.seekChainActive ? this.resumeAfterSeek
      : (libentry.getState() === STATE_PLAYING);
    this.seekChainActive = true;

    this.canSeek = false;
    this.canSeekUntil = Date.now() + SEEK_LOCK_TIMEOUT_MS;
    libentry.seek(Math.round(targetMs));
  }

  /** 一串 seek 结束（落地或超时）：如果原本在播，把 op_seek_ms 打断的播放状态补回来 */
  private finishSeekChain(): void {
    if (!this.seekChainActive) return;
    this.seekChainActive = false;
    if (!this.resumeAfterSeek) return;
    this.resumeAfterSeek = false;
    if (libentry.getState() !== STATE_PLAYING) {
      libentry.play();
    }
  }

  private pin(targetMs: number): void {
    const now = Date.now();
    this.seekTargetMs = targetMs;
    this.seekHoldUntil = now + SEEK_UI_HOLD_MS;
    this.seekActiveUntil = now + SEEK_ACTIVE_WINDOW_MS;
  }

  private display(ms: number): void {
    if (this.dragging) return;
    this.onDisplay(ms);
  }

  /* ------------------------------------------------------------ 进度条拖动 */

  public onSliderBegin(): void {
    this.dragging = true;
  }

  public onSliderMoving(ms: number): void {
    this.onDisplay(ms);
  }

  public onSliderEnd(ms: number): void {
    this.dragging = false;
    if (!this.initialized) return;

    this.clearTimer();
    this.pendingSeekMs = -1;

    if (this.canSeek) {
      this.postSeek(ms);
    } else {
      this.pendingSeekMs = ms;
      this.display(ms);
      this.pin(ms);
    }
  }

  /* ------------------------------------------------------------ 事件 */

  private handleEvent(event: PlayerEvent): void {
    switch (event.type) {
      case EVENT_OPENED:
        this.handleOpened(event);
        break;
      case EVENT_POSITION:
        this.handlePosition(event.positionMs);
        break;
      case EVENT_STATE:
        // seek 期间 op_seek_ms 可能临时暂停，别让播放图标跟着闪一下
        if (this.seekChainActive && event.state === STATE_PAUSED) break;
        this.onState(event.state);
        break;
      case EVENT_SEEK_DONE:
        this.handleSeekDone(event.positionMs);
        break;
      case EVENT_COMPLETED:
        this.onCompleted();
        break;
      case EVENT_ERROR:
        this.onError(event.message.length > 0 ? event.message : '未知错误');
        break;
      default:
        break;
    }
  }

  private handleOpened(event: PlayerEvent): void {
    const info = new MediaInfo();
    info.durationMs = event.durationMs;
    info.width = event.width;
    info.height = event.height;
    info.hasVideo = event.hasVideo === 1;
    info.hasAudio = event.hasAudio === 1;
    info.isStream = event.isStream === 1;
    info.isMusic = event.isMusic === 1;
    info.isAudioMedia = event.isAudioMedia === 1;

    this.durationMs = info.durationMs;
    this.positionMs = 0;
    this.initialized = true;
    this.streamMode = info.isStream;
    this.resetSeekState();
    this.onDisplay(0);

    // 换片源后以 SDK 的当前倍速为准（流媒体会被内核强制回 1.0）
    this.speed = libentry.getSpeedX10() / 10;
    if (this.speed <= 0) {
      this.speed = 1.0;
    }

    // 与 Qt 壳一致：打开成功、首帧就绪后自动开播
    libentry.play();
    this.onOpened(info);
  }

  private handlePosition(positionMs: number): void {
    this.positionMs = positionMs;
    const now = Date.now();

    if (this.seekTargetMs >= 0) {
      const diff = Math.abs(positionMs - this.seekTargetMs);
      const landed = diff <= SEEK_LAND_TOLERANCE_MS;
      if (!landed && now < this.seekHoldUntil) return;
      this.seekTargetMs = -1;
      this.seekHoldUntil = 0;
    }

    // 兜底：SEEK_DONE 丢了也不能把快进/快退永久锁死
    if (!this.canSeek && now >= this.canSeekUntil) {
      this.canSeek = true;
      this.finishSeekChain();
    }

    this.display(positionMs);
  }

  private handleSeekDone(positionMs: number): void {
    this.canSeek = true;
    this.canSeekUntil = 0;

    if (this.pendingSeekMs >= 0) {
      const target = this.pendingSeekMs;
      this.pendingSeekMs = -1;
      this.postSeek(target);
      return;
    }

    const now = Date.now();
    this.positionMs = positionMs;

    // 先恢复原播放状态（连续快进时下面会早退，不能漏掉这一步）
    this.finishSeekChain();

    // 用户还在连续 seek：进度条由按键驱动，别用落点去抖
    if (now < this.seekActiveUntil) return;

    this.seekTargetMs = positionMs;
    this.seekHoldUntil = now + SEEK_UI_HOLD_MS;
    this.display(positionMs);
  }

  private resetSeekState(): void {
    this.clearTimer();
    this.canSeek = true;
    this.canSeekUntil = 0;
    this.pendingSeekMs = -1;
    this.seekTargetMs = -1;
    this.seekHoldUntil = 0;
    this.seekActiveUntil = 0;
    this.dragging = false;
    this.seekChainActive = false;
    this.resumeAfterSeek = false;
  }

  private clearTimer(): void {
    if (this.timerId >= 0) {
      clearTimeout(this.timerId);
      this.timerId = -1;
    }
  }
}
