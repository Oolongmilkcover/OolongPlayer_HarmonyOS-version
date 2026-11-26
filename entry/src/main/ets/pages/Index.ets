import { common } from '@kit.AbilityKit';
import { window } from '@kit.ArkUI';
import { BusinessError } from '@kit.BasicServicesKit';
import { fileIo as fs, picker } from '@kit.CoreFileKit';
import {
  MediaInfo,
  PlayerController,
  STATE_ENDED,
  STATE_PAUSED,
  STATE_PLAYING,
  STATE_STOPPED
} from '../model/PlayerController';

const COLOR_BG: string = '#000000';
const COLOR_BAR: string = '#E61A1A1A';
const COLOR_TEXT: string = '#F0F0F0';
const COLOR_SUBTEXT: string = '#8F8F8F';
const COLOR_ACCENT: string = '#FF7043';

class PlayItem {
  name: string;
  uri: string;

  constructor(name: string, uri: string) {
    this.name = name;
    this.uri = uri;
  }
}

@Entry
@Component
struct Index {
  @State private playing: boolean = false;
  @State private initialized: boolean = false;
  @State private opening: boolean = false;
  @State private videoVisible: boolean = false;
  @State private isStream: boolean = false;
  @State private durationMs: number = 0;
  @State private displayMs: number = 0;
  @State private speed: number = 1.0;
  @State private volume: number = 0.5;
  @State private mediaName: string = '';
  @State private playlist: PlayItem[] = [];
  @State private currentIndex: number = -1;
  @State private playlistVisible: boolean = false;
  @State private fullscreen: boolean = false;
  @State private controlsVisible: boolean = true;
  @State private showUrlDialog: boolean = false;
  @State private urlText: string = '';
  @State private showError: boolean = false;
  @State private errorMessage: string = '';
  @State private surfaceW: number = 0;
  @State private surfaceH: number = 0;

  private controller: PlayerController = new PlayerController();
  private videoController: XComponentController = new XComponentController();
  private currentFile: fs.File | null = null;
  private lastVolume: number = 0.5;
  private openMenu: MenuElement[] = [];
  private filterMenu: MenuElement[] = [];
  private hideTimer: number = -1;
  private hideToken: number = 0;
  @State private holding: boolean = false;
  private speedBeforeHold: number = 1.0;
  private longPressTimer: number = -1;
  private longPressed: boolean = false;
  private singleTapTimer: number = -1;
  private touchDownX: number = 0;
  private refreshTimer: number = -1;

  aboutToAppear(): void {
    this.openMenu = [
      {
        value: '打开文件',
        action: (): void => {
          this.pickFiles(true);
        }
      },
      {
        value: '打开链接',
        action: (): void => {
          this.urlText = '';
          this.showUrlDialog = true;
        }
      }
    ];
    this.filterMenu = [
      { value: '原色', action: (): void => this.applyFilter(0) },
      { value: '灰度', action: (): void => this.applyFilter(1) },
      { value: '反色', action: (): void => this.applyFilter(2) },
      { value: '暖色', action: (): void => this.applyFilter(3) },
      { value: '冷色', action: (): void => this.applyFilter(4) }
    ];

    this.controller.onOpened = (info: MediaInfo): void => {
      this.opening = false;
      this.initialized = true;
      this.playing = true;
      this.durationMs = info.durationMs;
      this.displayMs = 0;
      this.isStream = info.isStream;
      this.videoVisible = info.hasVideo;
      this.speed = this.controller.getSpeed();
      this.pokeControls();
    };
    this.controller.onDisplay = (positionMs: number): void => {
      this.displayMs = positionMs;
    };
    this.controller.onCompleted = (): void => {
      this.playNext();
    };
    this.controller.onError = (message: string): void => {
      this.opening = false;
      this.playing = false;
      this.videoVisible = false;
      this.showErrorMsg(message);
    };
    // 点击时先本地翻转，SDK 状态事件回来后再校正，避免图标与实际状态长期不一致
    this.controller.onState = (state: number): void => {
      if (state === STATE_PLAYING) {
        this.playing = true;
      } else if (state === STATE_PAUSED || state === STATE_STOPPED || state === STATE_ENDED) {
        this.playing = false;
      }
    };

    this.controller.init();
  }

  aboutToDisappear(): void {
    this.cancelHide();
    this.clearLongPressTimer();
    this.clearRefreshTimer();
    if (this.singleTapTimer !== -1) {
      clearTimeout(this.singleTapTimer);
      this.singleTapTimer = -1;
    }
    if (this.fullscreen) {
      this.applyImmersive(false);
      this.applyOrientation(false);
    }
    this.controller.detachSurface();
    this.closeCurrentFile();
    this.controller.release();
  }

  build() {
    Stack() {
      this.VideoLayer()
      if (!this.videoVisible) {
        this.BlackLayer()
      }
      this.GestureLayer()
      if (this.holding) {
        Column() {
          Text('2.0x 倍速')
            .fontSize(13)
            .fontColor(Color.White)
            .backgroundColor('#88000000')
            .borderRadius(14)
            .padding({ left: 12, right: 12, top: 5, bottom: 5 })
        }
        .width('100%')
        .height('100%')
        .justifyContent(FlexAlign.Start)
        .padding({ top: 46 })
        .hitTestBehavior(HitTestMode.None)
      }
      Column() {
        if (this.controlsVisible) {
          this.TopBar()
        }
        Blank()
          .hitTestBehavior(HitTestMode.None)
        if (this.controlsVisible) {
          this.ControlBar()
        }
      }
      .width('100%')
      .height('100%')
      .hitTestBehavior(HitTestMode.None)

      if (this.playlistVisible) {
        this.PlaylistPanel()
      }
      if (this.showUrlDialog) {
        this.UrlDialog()
      }
      if (this.showError) {
        this.ErrorDialog()
      }
    }
    .width('100%')
    .height('100%')
    .backgroundColor(COLOR_BG)
  }

  /* ------------------------------------------------------------ 构建块 */

  @Builder
  VideoLayer() {
    XComponent({
      id: 'oplayer_video',
      type: XComponentType.SURFACE,
      controller: this.videoController
    })
      .width('100%')
      .height('100%')
      .onLoad(() => {
        this.attachSurface();
      })
      .onDestroy(() => {
        this.controller.detachSurface();
      })
      .onAreaChange((oldArea: Area, newArea: Area) => {
        const width = newArea.width as number;
        const height = newArea.height as number;
        const changed = width !== this.surfaceW || height !== this.surfaceH;
        this.surfaceW = width;
        this.surfaceH = height;
        this.resizeSurface();
        // 尺寸（进/出全屏、旋转）真的变了：等新表面尺寸稳定后主动重绘一帧，
        // 暂停时没有新帧也不会停在旧比例
        if (changed) {
          this.scheduleRefreshSurface();
        }
      })
  }

  @Builder
  BlackLayer() {
    Column() {
      Image($r('app.media.oplayer_logo'))
        .width(92)
        .height(92)
        .borderRadius(20)
        .opacity(0.92)
      Text('OPlayer')
        .fontSize(22)
        .fontWeight(FontWeight.Bold)
        .fontColor(COLOR_TEXT)
        .margin({ top: 16 })
      if (!this.initialized) {
        Text(this.opening ? '正在打开…' : '选择本地文件或网络链接开始播放')
          .fontSize(13)
          .fontColor(COLOR_SUBTEXT)
          .margin({ top: 8 })
      } else {
        Text('纯音频播放中')
          .fontSize(13)
          .fontColor(COLOR_SUBTEXT)
          .margin({ top: 8 })
      }
    }
    .width('100%')
    .height('100%')
    .justifyContent(FlexAlign.Center)
    .backgroundColor(COLOR_BG)
  }

  @Builder
  GestureLayer() {
    Column()
      .width('100%')
      .height('100%')
      .backgroundColor(Color.Transparent)
      .onTouch((event: TouchEvent): void => {
        this.onStageTouch(event);
      })
  }

  @Builder
  TopBar() {
    Row() {
      Image($r('app.media.oplayer_logo'))
        .width(24)
        .height(24)
        .borderRadius(12)
        .flexShrink(0)
      Text('OPlayer')
        .fontSize(16)
        .fontWeight(FontWeight.Bold)
        .fontColor(COLOR_TEXT)
        .margin({ left: 6 })
        .flexShrink(0)
      Text(this.playTitle())
        .fontSize(12)
        .fontColor(COLOR_SUBTEXT)
        .maxLines(1)
        .textOverflow({ overflow: TextOverflow.Ellipsis })
        .layoutWeight(1)
        .constraintSize({ minWidth: 0 })
        .margin({ left: 10, right: 6 })
      Row({ space: 4 }) {
        Image($r('app.media.open_file_btn'))
          .width(18)
          .height(18)
          .objectFit(ImageFit.Contain)
        Text('打开')
          .fontSize(12)
          .fontColor('#1A1A1A')
      }
      .height(32)
      .padding({ left: 10, right: 14 })
      .borderRadius(16)
      .backgroundColor('#F2F2F2')
      .flexShrink(0)
      .bindMenu(this.openMenu)
    }
    .width('100%')
    .height(52)
    .padding({ left: 16, right: 14 })
    .alignItems(VerticalAlign.Center)
    .backgroundColor(COLOR_BAR)
  }

  @Builder
  ControlBar() {
    Column() {
      Row() {
        Text(this.formatTime(this.displayMs))
          .fontSize(11)
          .fontColor(COLOR_SUBTEXT)
          .width(56)
        Slider({
          value: this.displayMs,
          min: 0,
          max: this.sliderMax(),
          step: 1
        })
          .layoutWeight(1)
          .enabled(this.initialized && !this.isStream)
          .blockColor('#FFFFFF')
          .trackColor('#3A3A3A')
          .selectedColor(COLOR_ACCENT)
          .onChange((value: number, mode: SliderChangeMode) => {
            this.onProgressChange(value, mode);
          })
        Text(this.formatTime(this.durationMs))
          .fontSize(11)
          .fontColor(COLOR_SUBTEXT)
          .width(56)
          .textAlign(TextAlign.End)

        Button({ type: ButtonType.Circle }) {
          Image(this.volume > 0.001 ? $r('app.media.volume_btn') : $r('app.media.volume_none_btn'))
            .width(18)
            .height(18)
            .objectFit(ImageFit.Contain)
        }
        .width(30)
        .height(30)
        .backgroundColor(Color.Transparent)
        .onClick(() => {
          this.toggleMute();
        })

        Slider({
          value: this.volume,
          min: 0,
          max: 1,
          step: 0.01
        })
          .width(62)
          .blockColor('#FFFFFF')
          .trackColor('#3A3A3A')
          .selectedColor(COLOR_ACCENT)
          .onChange((value: number) => {
            this.setVolume(value);
          })
      }
      .width('100%')
      .height(30)
      .padding({ left: 8, right: 8 })

      Scroll() {
        Row() {
          // 播放/暂停：点击时本地立即翻转图标（乐观更新），SDK 状态事件再校正
          Button({ type: ButtonType.Circle }) {
            Image(this.playing ? $r('app.media.pause_btn') : $r('app.media.start_btn'))
              .width(24)
              .height(24)
              .objectFit(ImageFit.Contain)
              .colorFilter([
                0, 0, 0, 0, 255,
                0, 0, 0, 0, 255,
                0, 0, 0, 0, 255,
                0, 0, 0, 1, 0
              ])
          }
          .width(36)
          .height(36)
          .backgroundColor(Color.Transparent)
          .enabled(this.initialized)
          .opacity(this.initialized ? 1 : 0.35)
          .onClick((): void => {
            this.togglePlay();
          })
          this.IconButton($r('app.media.stop_btn'), this.initialized, (): void => {
            this.stopPlayback();
          })
          this.IconButton($r('app.media.prev_btn'), this.canSwitchTrack(), (): void => {
            this.playPrev();
          })
          this.IconButton($r('app.media.next_btn'), this.canSwitchTrack(), (): void => {
            this.playNext();
          })
          this.IconButton($r('app.media.rewind_btn'), this.canSeekUi(), (): void => {
            this.controller.rewind();
          })
          this.IconButton($r('app.media.ff_btn'), this.canSeekUi(), (): void => {
            this.controller.fastForward();
          })
          this.IconButton($r('app.media.slowdown_btn'), this.canSetSpeed(), (): void => {
            this.changeSpeed(-0.1);
          })
          Text('倍速:' + this.speed.toFixed(1))
            .fontSize(10)
            .fontColor('#CFCFCF')
            .width(42)
            .textAlign(TextAlign.Center)
          this.IconButton($r('app.media.speedup_btn'), this.canSetSpeed(), (): void => {
            this.changeSpeed(0.1);
          })
          Button({ type: ButtonType.Circle }) {
            Image($r('app.media.setting_btn'))
              .width(18)
              .height(18)
              .objectFit(ImageFit.Contain)
          }
          .width(28)
          .height(28)
          .backgroundColor(Color.Transparent)
          .bindMenu(this.filterMenu)

          this.IconButton($r('app.media.showlist_btn'), true, (): void => {
            this.playlistVisible = !this.playlistVisible;
          })
          this.IconButton($r('app.media.full_btn'), true, (): void => {
            this.toggleFullscreen();
          })
        }
        .height(40)
        .padding({ left: 4, right: 4 })
      }
      .scrollable(ScrollDirection.Horizontal)
      .scrollBar(BarState.Off)
      .width('100%')
    }
    .width('100%')
    .backgroundColor(COLOR_BAR)
    .padding({ top: 2, bottom: 8 })
  }

  @Builder
  IconButton(icon: Resource, enabled: boolean, action: () => void) {
    Button({ type: ButtonType.Circle }) {
      Image(icon)
        .width(18)
        .height(18)
        .objectFit(ImageFit.Contain)
    }
    .width(28)
    .height(28)
    .backgroundColor(Color.Transparent)
    .enabled(enabled)
    .opacity(enabled ? 1.0 : 0.35)
    .onClick(() => {
      action();
    })
  }

  @Builder
  PlaylistPanel() {
    Row() {
      Blank()
      Column() {
        Row() {
          Text('播放列表')
            .fontSize(16)
            .fontColor(COLOR_TEXT)
            .fontWeight(FontWeight.Medium)
            .layoutWeight(1)
          Button({ type: ButtonType.Circle }) {
            Image($r('app.media.open_file_btn'))
              .width(18)
              .height(18)
              .objectFit(ImageFit.Contain)
          }
          .width(32)
          .height(32)
          .backgroundColor(Color.Transparent)
          .onClick(() => {
            this.pickFiles(false);
          })
          Button({ type: ButtonType.Circle }) {
            Image($r('app.media.close_btn'))
              .width(18)
              .height(18)
              .objectFit(ImageFit.Contain)
          }
          .width(32)
          .height(32)
          .backgroundColor(Color.Transparent)
          .onClick(() => {
            this.playlistVisible = false;
          })
        }
        .width('100%')
        .height(48)
        .padding({ left: 16, right: 8 })

        if (this.playlist.length === 0) {
          Text('列表为空\n点击右上角按钮添加文件')
            .fontSize(13)
            .fontColor(COLOR_SUBTEXT)
            .textAlign(TextAlign.Center)
            .width('100%')
            .margin({ top: 80 })
        } else {
          List({ space: 2 }) {
            ForEach(this.playlist, (item: PlayItem, index: number) => {
              ListItem() {
                Row() {
                  Text((index + 1).toString())
                    .fontSize(11)
                    .fontColor('#7A7A7A')
                    .width(26)
                  Text(item.name)
                    .fontSize(13)
                    .fontColor('#E0E0E0')
                    .maxLines(1)
                    .textOverflow({ overflow: TextOverflow.Ellipsis })
                    .layoutWeight(1)
                }
                .width('100%')
                .height(44)
                .padding({ left: 8, right: 12 })
                .backgroundColor(index === this.currentIndex ? '#33FF7043' : Color.Transparent)
                .onClick(() => {
                  this.playIndex(index);
                })
                .bindContextMenu(this.itemMenu(index), ResponseType.LongPress)
              }
            }, (item: PlayItem, index: number) => index.toString() + '_' + item.uri)
          }
          .width('100%')
          .layoutWeight(1)
        }
      }
      .width(this.fullscreen ? 300 : '72%')
      .height('100%')
      .backgroundColor('#F21C1C1C')
    }
    .width('100%')
    .height('100%')
    .hitTestBehavior(HitTestMode.None)
  }

  @Builder
  itemMenu(index: number) {
    Menu() {
      MenuItem({ content: '播放' })
        .onClick(() => {
          this.playIndex(index);
        })
      MenuItem({ content: '删除' })
        .onClick(() => {
          this.removeIndex(index);
        })
    }
  }

  @Builder
  UrlDialog() {
    Column() {
      Column() {
        Text('打开链接')
          .fontSize(18)
          .fontWeight(FontWeight.Bold)
          .fontColor(COLOR_TEXT)
        TextInput({ text: this.urlText, placeholder: 'rtmp:// http:// https://' })
          .width('100%')
          .height(44)
          .margin({ top: 16 })
          .backgroundColor('#1A1A1A')
          .fontColor(COLOR_TEXT)
          .placeholderColor('#707070')
          .onChange((value: string) => {
            this.urlText = value;
          })
        Row() {
          Button('取消')
            .layoutWeight(1)
            .height(40)
            .backgroundColor('#3A3A3A')
            .fontColor(COLOR_TEXT)
            .onClick(() => {
              this.showUrlDialog = false;
            })
          Button('打开')
            .layoutWeight(1)
            .height(40)
            .margin({ left: 12 })
            .backgroundColor(COLOR_ACCENT)
            .fontColor('#FFFFFF')
            .onClick(() => {
              this.openUrl();
            })
        }
        .width('100%')
        .margin({ top: 20 })
      }
      .width('85%')
      .padding(20)
      .borderRadius(16)
      .backgroundColor('#2A2A2A')
    }
    .width('100%')
    .height('100%')
    .justifyContent(FlexAlign.Center)
    .backgroundColor('#99000000')
  }

  @Builder
  ErrorDialog() {
    Column() {
      Column() {
        Text('错误')
          .fontSize(18)
          .fontWeight(FontWeight.Bold)
          .fontColor(COLOR_TEXT)
        Text(this.errorMessage)
          .fontSize(14)
          .fontColor('#CCCCCC')
          .textAlign(TextAlign.Center)
          .margin({ top: 12 })
        Button('确定')
          .width('100%')
          .height(40)
          .margin({ top: 20 })
          .backgroundColor(COLOR_ACCENT)
          .fontColor('#FFFFFF')
          .onClick(() => {
            this.showError = false;
          })
      }
      .width('80%')
      .padding(20)
      .borderRadius(16)
      .backgroundColor('#2A2A2A')
    }
    .width('100%')
    .height('100%')
    .justifyContent(FlexAlign.Center)
    .backgroundColor('#99000000')
  }

  /* ------------------------------------------------------------ 表面 */

  private attachSurface(): void {
    const surfaceId = this.videoController.getXComponentSurfaceId();
    if (surfaceId.length === 0) return;
    this.controller.attachSurface(surfaceId, this.toPx(this.surfaceW), this.toPx(this.surfaceH));
  }

  private resizeSurface(): void {
    if (this.surfaceW <= 0 || this.surfaceH <= 0) return;
    this.controller.resizeSurface(this.toPx(this.surfaceW), this.toPx(this.surfaceH));
  }

  private scheduleRefreshSurface(): void {
    this.clearRefreshTimer();
    this.refreshTimer = setTimeout((): void => {
      this.refreshTimer = -1;
      this.refreshSurface();
    }, 120);
  }

  /** 按当前组件尺寸重建渲染表面并重绘最后一帧 */
  private refreshSurface(): void {
    if (this.surfaceW <= 0 || this.surfaceH <= 0) return;
    this.controller.refreshSurface(this.toPx(this.surfaceW), this.toPx(this.surfaceH));
  }

  private clearRefreshTimer(): void {
    if (this.refreshTimer !== -1) {
      clearTimeout(this.refreshTimer);
      this.refreshTimer = -1;
    }
  }

  private toPx(value: number): number {
    return Math.round(this.getUIContext().vp2px(value));
  }

  /* ------------------------------------------------------------ 手势与自动隐藏 */

  private onStageTouch(event: TouchEvent): void {
    if (event.type === TouchType.Down) {
      if (event.touches.length > 0) {
        this.touchDownX = event.touches[0].x;
      }
      this.clearLongPressTimer();
      this.longPressed = false;
      this.longPressTimer = setTimeout((): void => {
        this.longPressTimer = -1;
        this.longPressed = true;
        this.startHoldSpeed();
      }, 450);
      return;
    }
    if (event.type === TouchType.Up) {
      this.clearLongPressTimer();
      if (this.longPressed) {
        this.longPressed = false;
        this.endHoldSpeed();
        return;
      }
      if (this.singleTapTimer !== -1) {
        clearTimeout(this.singleTapTimer);
        this.singleTapTimer = -1;
        this.handleDoubleTapAt(this.touchDownX);
      } else {
        this.singleTapTimer = setTimeout((): void => {
          this.singleTapTimer = -1;
          this.handleSingleTap();
        }, 400);
      }
      return;
    }
    if (event.type === TouchType.Cancel) {
      this.clearLongPressTimer();
      if (this.longPressed) {
        this.longPressed = false;
        this.endHoldSpeed();
      }
    }
  }

  private clearLongPressTimer(): void {
    if (this.longPressTimer !== -1) {
      clearTimeout(this.longPressTimer);
      this.longPressTimer = -1;
    }
  }

  private handleSingleTap(): void {
    if (this.controlsVisible) {
      this.cancelHide();
      this.controlsVisible = false;
    } else {
      this.pokeControls();
    }
  }

  private handleDoubleTapAt(x: number): void {
    if (!this.fullscreen) {
      // 竖屏保持原行为：双击暂停/播放
      this.handleDoubleTap();
      return;
    }
    // 全屏分三块热区：左 1/5 快退、右 1/5 快进、中间 3/5 暂停/播放
    const w = this.surfaceW > 0 ? this.surfaceW : 360;
    const zone = x < w * 0.2 ? 'left' : (x > w * 0.8 ? 'right' : 'mid');
    if (zone === 'left') {
      if (this.canSeekUi()) {
        this.controller.rewind();
      }
      this.pokeControls();
      return;
    }
    if (zone === 'right') {
      if (this.canSeekUi()) {
        this.controller.fastForward();
      }
      this.pokeControls();
      return;
    }
    this.handleDoubleTap();
  }

  private handleDoubleTap(): void {
    this.togglePlay();
    this.pokeControls();
  }

  private startHoldSpeed(): void {
    if (!this.canSetSpeed() || this.holding) return;
    this.holding = true;
    this.speedBeforeHold = this.speed;
    this.setSpeedTo(2.0);
  }

  private endHoldSpeed(): void {
    if (!this.holding) return;
    this.holding = false;
    this.setSpeedTo(this.speedBeforeHold);
  }

  private setSpeedTo(target: number): void {
    const delta = target - this.speed;
    if (Math.abs(delta) < 0.05) return;
    this.speed = this.controller.changeSpeed(delta);
  }

  private pokeControls(): void {
    this.controlsVisible = true;
    this.scheduleHide();
  }

  private cancelHide(): void {
    this.hideToken++;
    if (this.hideTimer !== -1) {
      clearTimeout(this.hideTimer);
      this.hideTimer = -1;
    }
  }

  private scheduleHide(): void {
    this.cancelHide();
    if (!this.initialized) return;
    const token = this.hideToken;
    this.hideTimer = setTimeout((): void => {
      if (token !== this.hideToken) return;
      if (this.playlistVisible || this.showUrlDialog || this.showError) return;
      this.controlsVisible = false;
    }, 3000);
  }

  /* ------------------------------------------------------------ 播放控制 */

  private togglePlay(): void {
    if (!this.initialized) return;
    this.pokeControls();
    if (this.playing) {
      this.controller.pause();
      this.playing = false;
    } else {
      this.controller.play();
      this.playing = true;
    }
  }

  private stopPlayback(): void {
    if (!this.initialized) return;
    this.controller.stop();
    this.playing = false;
    this.initialized = false;
    this.videoVisible = false;
    this.isStream = false;
    this.durationMs = 0;
    this.displayMs = 0;
    this.mediaName = '';
    this.currentIndex = -1;
    this.cancelHide();
    this.controlsVisible = true;
    this.closeCurrentFile();
  }

  private changeSpeed(delta: number): void {
    if (!this.initialized || this.isStream) return;
    this.pokeControls();
    this.speed = this.controller.changeSpeed(delta);
  }

  private setVolume(value: number): void {
    let volume = value;
    if (volume < 0) volume = 0;
    if (volume > 1) volume = 1;
    this.volume = volume;
    this.controller.setVolume(volume);
    this.pokeControls();
  }

  private toggleMute(): void {
    if (this.volume > 0.001) {
      this.lastVolume = this.volume;
      this.setVolume(0);
    } else {
      this.setVolume(this.lastVolume > 0.001 ? this.lastVolume : 0.5);
    }
  }

  private applyFilter(type: number): void {
    this.controller.setFilter(type);
    this.pokeControls();
  }

  private toggleFullscreen(): void {
    if (!this.initialized) return;
    this.fullscreen = !this.fullscreen;
    if (this.fullscreen) {
      this.playlistVisible = false;
    }
    this.applyOrientation(this.fullscreen);
    this.applyImmersive(this.fullscreen);
    // 旋转完成后重新绑定表面，并按新方向主动重绘一帧（暂停时不会再有新帧）
    setTimeout((): void => {
      this.controller.detachSurface();
      this.attachSurface();
      this.refreshSurface();
      setTimeout((): void => {
        this.attachSurface();
        this.refreshSurface();
        setTimeout((): void => {
          this.attachSurface();
          this.refreshSurface();
        }, 600);
      }, 500);
    }, 700);
    this.pokeControls();
  }

  private applyOrientation(landscape: boolean): void {
    try {
      const context = getContext(this) as common.UIAbilityContext;
      window.getLastWindow(context).then((win: window.Window) => {
        win.setPreferredOrientation(landscape ? window.Orientation.LANDSCAPE : window.Orientation.PORTRAIT);
      }).catch((err: BusinessError) => {
        console.error('setPreferredOrientation failed: ' + JSON.stringify(err));
      });
    } catch (err) {
      const error = err as BusinessError;
      console.error('orientation failed: ' + error.message);
    }
  }

  private applyImmersive(enable: boolean): void {
    try {
      const context = getContext(this) as common.UIAbilityContext;
      window.getLastWindow(context).then((win: window.Window) => {
        win.setWindowLayoutFullScreen(enable);
        const bars: Array<'status' | 'navigation'> = enable ? [] : ['status', 'navigation'];
        win.setWindowSystemBarEnable(bars);
      }).catch((err: BusinessError) => {
        console.error('setWindowLayoutFullScreen failed: ' + JSON.stringify(err));
      });
    } catch (err) {
      const error = err as BusinessError;
      console.error('immersive failed: ' + error.message);
    }
  }

  /* ------------------------------------------------------------ 进度 */

  private onProgressChange(value: number, mode: SliderChangeMode): void {
    this.pokeControls();
    if (mode === SliderChangeMode.Begin) {
      this.controller.onSliderBegin();
      return;
    }
    if (mode === SliderChangeMode.Moving) {
      this.displayMs = value;
      this.controller.onSliderMoving(value);
      return;
    }
    this.displayMs = value;
    this.controller.onSliderEnd(value);
  }

  private sliderMax(): number {
    return this.durationMs > 0 ? this.durationMs : 1;
  }

  private canSeekUi(): boolean {
    return this.initialized && !this.isStream;
  }

  private canSetSpeed(): boolean {
    return this.initialized && !this.isStream;
  }

  private canSwitchTrack(): boolean {
    return this.initialized && !this.isStream && this.playlist.length > 1;
  }

  /* ------------------------------------------------------------ 播放列表 */

  private async pickFiles(playFirst: boolean): Promise<void> {
    try {
      const documentPicker = new picker.DocumentViewPicker(getContext(this));
      const options = new picker.DocumentSelectOptions();
      options.maxSelectNumber = 50;
      options.fileSuffixFilters = ['音视频|.mp4,.mkv,.avi,.mov,.flv,.ts,.m4v,.webm,.mp3,.flac,.aac,.wav,.m4a'];
      const uris = await documentPicker.select(options);
      if (uris === null || uris.length === 0) return;

      const additions: PlayItem[] = [];
      for (let i = 0; i < uris.length; i++) {
        additions.push(new PlayItem(this.fileNameOf(uris[i]), uris[i]));
      }
      const startIndex = this.playlist.length;
      this.playlist = this.playlist.concat(additions);

      if (playFirst) {
        this.playIndex(startIndex);
      }
    } catch (err) {
      this.showErrorMsg('选择文件失败：' + this.errorText(err as BusinessError));
    }
  }

  private async playIndex(index: number): Promise<void> {
    if (index < 0 || index >= this.playlist.length) return;
    const item = this.playlist[index];

    let file: fs.File | null = null;
    try {
      file = fs.openSync(item.uri, fs.OpenMode.READ_ONLY);
    } catch (err) {
      this.showErrorMsg('无法读取文件：' + this.errorText(err as BusinessError));
      return;
    }
    if (file === null) return;

    this.opening = true;
    // fd 打开时 path 只是扩展名提示，传文件名比传 URI 更干净
    const result = this.controller.open(item.name, file.fd);
    if (result !== 0) {
      this.opening = false;
      try {
        fs.closeSync(file);
      } catch (err) {
        const error = err as BusinessError;
        console.error('close fd failed: ' + error.message);
      }
      this.showErrorMsg('打开失败：' + this.openErrorText(result));
      return;
    }

    // 新文件已经接手，旧的 fd 可以关了
    this.closeCurrentFile();
    this.currentFile = file;
    this.currentIndex = index;
    this.mediaName = item.name;
    this.displayMs = 0;
    this.durationMs = 0;
    this.playing = false;
  }

  private openUrl(): void {
    const url = this.urlText.trim();
    if (!(url.startsWith('rtmp://') || url.startsWith('http://') || url.startsWith('https://'))) {
      this.showErrorMsg('不支持的链接格式，请使用 RTMP/HTTP/HTTPS。');
      return;
    }
    this.showUrlDialog = false;
    this.opening = true;

    const result = this.controller.open(url, -1);
    if (result !== 0) {
      this.opening = false;
      this.showErrorMsg('打开失败：' + this.openErrorText(result));
      return;
    }

    this.closeCurrentFile();
    this.currentIndex = -1;
    this.mediaName = url;
    this.displayMs = 0;
    this.durationMs = 0;
    this.playing = false;
  }

  private playNext(): void {
    if (this.isStream || this.playlist.length === 0) return;
    const count = this.playlist.length;
    const next = this.currentIndex < 0 ? 0 : (this.currentIndex + 1) % count;
    this.playIndex(next);
  }

  private playPrev(): void {
    if (this.isStream || this.playlist.length === 0) return;
    const count = this.playlist.length;
    const prev = this.currentIndex < 0 ? count - 1 : (this.currentIndex - 1 + count) % count;
    this.playIndex(prev);
  }

  private removeIndex(index: number): void {
    if (index < 0 || index >= this.playlist.length) return;
    const next = this.playlist.slice();
    next.splice(index, 1);
    this.playlist = next;

    if (index < this.currentIndex) {
      this.currentIndex = this.currentIndex - 1;
    } else if (index === this.currentIndex) {
      // 正在播的文件被移出列表：播放不停，但没有对应行了
      this.currentIndex = -1;
    }
  }

  private closeCurrentFile(): void {
    if (this.currentFile === null) return;
    try {
      fs.closeSync(this.currentFile);
    } catch (err) {
      const error = err as BusinessError;
      console.error('close fd failed: ' + error.message);
    }
    this.currentFile = null;
  }

  /* ------------------------------------------------------------ 工具 */

  private playTitle(): string {
    return this.mediaName.length > 0 ? '正在播放:' + this.mediaName : '暂未播放';
  }

  private fileNameOf(uri: string): string {
    const clean = uri.split('?')[0];
    const parts = clean.split('/');
    let name = parts.length > 0 ? parts[parts.length - 1] : uri;
    try {
      name = decodeURIComponent(name);
    } catch (err) {
      name = name;
    }
    return name.length > 0 ? name : uri;
  }

  private errorText(err: BusinessError): string {
    if (err !== null && err.message !== undefined && err.message.length > 0) {
      return err.message;
    }
    return '未知错误';
  }

  private openErrorText(code: number): string {
    if (code === -1) return '参数无效';
    if (code === -2) return '无法打开文件（不存在、格式不支持或网络失败）';
    if (code === -3) return '文件里没有可播放的音视频流';
    if (code === -4) return '当前平台不支持该编码';
    if (code === -5) return '播放器尚未就绪';
    if (code === -6) return '播放器忙，请稍后再试';
    if (code === -7) return '读写失败';
    if (code === -8) return '播放器内部错误';
    return '错误码 ' + code;
  }

  private showErrorMsg(message: string): void {
    this.errorMessage = message;
    this.showError = true;
  }

  private formatTime(ms: number): string {
    let totalSeconds = Math.floor(ms / 1000);
    if (totalSeconds < 0) totalSeconds = 0;
    const hours = Math.floor(totalSeconds / 3600);
    const minutes = Math.floor((totalSeconds % 3600) / 60);
    const seconds = totalSeconds % 60;
    return this.pad2(hours) + ':' + this.pad2(minutes) + ':' + this.pad2(seconds);
  }

  private pad2(value: number): string {
    return value < 10 ? '0' + value.toString() : value.toString();
  }
}
