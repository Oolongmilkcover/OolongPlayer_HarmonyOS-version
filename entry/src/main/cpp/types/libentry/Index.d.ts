/**
 * 原生播放器接口（libentry.so）
 *
 * 事件回调在 SDK 线程触发，原生层通过 threadsafe function 投递到 JS 线程，
 * 所以 ArkTS 里可以直接更新 UI 状态。
 */
export interface PlayerEvent {
  /** op_event_type：1 OPENED / 2 POSITION / 3 STATE / 4 SEEK_DONE / 5 BUFFERING / 6 COMPLETED / 7 ERROR */
  type: number;
  state: number;
  code: number;
  positionMs: number;
  message: string;
  durationMs: number;
  width: number;
  height: number;
  hasVideo: number;
  hasAudio: number;
  isStream: number;
  isMusic: number;
  isAudioMedia: number;
}

export const init: (callback: (event: PlayerEvent) => void) => void;
export const destroy: () => void;

/** XComponent 表面（surfaceId 来自 XComponentController.getXComponentSurfaceId()） */
export const attachSurface: (surfaceId: string, width: number, height: number) => boolean;
export const resizeSurface: (width: number, height: number) => void;
/** 尺寸变化后主动重建渲染表面并重绘最后一帧（暂停时也能立刻刷新比例） */
export const refreshSurface: (width: number, height: number) => void;
export const detachSurface: () => void;

/** path 是扩展名提示，fd >= 0 时优先用 fd（鸿蒙 picker 返回的 URI 先 fs.open 拿 fd） */
export const open: (path: string, fd: number, offset: number, size: number) => number;
export const play: () => number;
export const pause: () => number;
export const stop: () => number;
export const seek: (positionMs: number) => number;
export const setSpeed: (speedX10: number) => number;
export const setFilter: (type: number) => void;

export const getDuration: () => number;
export const getPosition: () => number;
export const getWidth: () => number;
export const getHeight: () => number;
export const getState: () => number;
export const hasVideo: () => number;
export const hasAudio: () => number;
export const isStream: () => number;
export const isMusic: () => number;
export const isAudioMedia: () => number;
export const getSpeedX10: () => number;
export const isMobile: () => number;
