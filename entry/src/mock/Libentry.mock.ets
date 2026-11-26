// 预览/单测用的原生层 mock：接口与 Index.d.ts 保持一致，不产生真实播放。
const NativeMock: Record<string, Object> = {
  'init': (callback: (event: Object) => void) => {
  },
  'destroy': () => {
  },
  'attachSurface': (surfaceId: string, width: number, height: number) => true,
  'resizeSurface': (width: number, height: number) => {
  },
  'refreshSurface': (width: number, height: number) => {
  },
  'detachSurface': () => {
  },
  'open': (path: string, fd: number, offset: number, size: number) => 0,
  'play': () => 0,
  'pause': () => 0,
  'stop': () => 0,
  'seek': (positionMs: number) => 0,
  'setSpeed': (speedX10: number) => 0,
  'setFilter': (type: number) => {
  },
  'getDuration': () => 0,
  'getPosition': () => 0,
  'getWidth': () => 0,
  'getHeight': () => 0,
  'getState': () => 0,
  'hasVideo': () => 0,
  'hasAudio': () => 0,
  'isStream': () => 0,
  'isMusic': () => 0,
  'isAudioMedia': () => 0,
  'getSpeedX10': () => 10,
  'isMobile': () => 1
};

export default NativeMock;
