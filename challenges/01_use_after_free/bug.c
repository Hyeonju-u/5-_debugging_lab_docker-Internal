/*
 * Challenge 01 — Use After Free (심화: vtable 기반 위젯 시스템)
 *
 * [시나리오]
 *   아주 작은 GUI 흉내. 각 위젯(Widget)은 힙 객체이며 첫 멤버로 "vtable"
 *   (render/on_event 함수 포인터 묶음)을 가진다. Screen 은 위젯 포인터 배열을
 *   들고 있고, 이벤트를 나눠준 뒤(dispatch) 한 프레임을 그린다(render).
 *
 * [기대 동작]
 *   버튼/라벨/다이얼로그를 그리고, 닫기 이벤트 후 남은 위젯만 다시 그린 뒤
 *   정상 종료(0).
 *
 * [증상]
 *   닫기 이벤트 핸들러가 다이얼로그 위젯을 free() 하지만, Screen 의 포인터 배열에서
 *   그 슬롯을 제거(NULL 로)하지 않는다. 그 사이 앱이 상태 메시지 버퍼를 새로 할당하며
 *   방금 해제된 청크를 재사용해 vtable 포인터 자리를 덮어쓴다.
 *   다음 렌더 패스에서 해제된 위젯의 w->vtbl->render 를 호출 → 망가진 함수 포인터로
 *   점프 → SIGSEGV. 크래시는 render 루프에서 나지만, 원인은 멀리 떨어진 close 핸들러다.
 *
 * [gdb 로 잡기]
 *   make gdb NAME=01_use_after_free
 *   (gdb) run                         → 크래시(SIGSEGV)
 *   (gdb) bt                          → screen_render() 안 w->vtbl->render(w) 지점
 *   (gdb) print w                     → 어떤 위젯인지(주소/슬롯) 확인
 *   (gdb) print w->vtbl               → 오염돼 있음
 *   (gdb) print s->items[2]           → 이미 해제된 슬롯이 그대로 남아있음
 *   (gdb) break widget_destroy        → 누가/언제 이 위젯을 free 하는지 역추적
 *
 * [printf(로그)로 잡기]
 *   위젯 해제 시점과 렌더 시점의 vtbl 값을 각각 찍어 "해제가 사용보다 먼저"인지 확인:
 *     (destroy) fprintf(stderr, "destroy id=%d w=%p vtbl=%p\n", w->id,(void*)w,(void*)w->vtbl);
 *     (render)  fprintf(stderr, "render  id=%d w=%p vtbl=%p\n", w->id,(void*)w,(void*)w->vtbl);
 *   → 같은 주소가 destroy 후 render 에서 다시 나오고, vtbl 값이 달라져 있으면 UAF.
 *   (stdout 은 버퍼링되니 stderr 로 찍어야 크래시 직전 로그가 남는다)
 *
 * TODO: "해제"와 "슬롯 정리"를 한 곳에서 같이 하세요. 위젯 자신은 Screen 을 모르므로
 *       (dialog_on_event 는 self 만 안다) 이벤트 핸들러에서는 closed 표시만 남기고,
 *       Screen 쪽에서 closed 위젯을 free 한 뒤 그 슬롯을 NULL 로 만드는 편이 자연스럽습니다.
 *       이후 dispatch/render 루프가 NULL 슬롯을 건너뛰게 하세요. "해제 = 소유 포인터 무효화".
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
// 위젯 구조체를 아직 정의하기 전에 이런 이름의 구조체가 있다라고 미리 알려주는 것 이렇게 해야 아래 VTtable 안에서 위젯을 쓸수있음 서로가 서로를 참조하는 구조라서
typedef struct Widget Widget;

typedef struct
{
    void (*render)(Widget *self);             // 그리기 함수를 가리키는 포인터
    void (*on_event)(Widget *self, int code); // 이벤트 처리 함수를 가리키는 포인터
} VTable;                                     // 함수 포인터들의 묶음 버튼,라벨,다이얼로그마다 그려지는 방식과 이벤트 처리 방식이 다른데 그 차이를 이 구조체 하나로 표현함 객체지향 언어의 다형성을 c에서 흉내내는 방법
// 실제 위젯 하나의 데이터 버튼이든 라벨이든 다이얼로그든 다 이 구조체를 씀
struct Widget
{
    const VTable *vtbl; // 이 위젯이 어떤 종류인지 버튼인지 다이얼로그인지 결정하는 함수 묶음
    int id;             // 위젯 고유 번호(구분용)
    int closed;         // 0이면 아직 살아있음 1이면 닫혔다고 표현된 상태
    char label[24];     // 화면에 표시할 텍스트
};

#define MAX_WIDGETS 8 // 위젯들을 담아두는 화면 하나 위젯 포인터를 최대 8개 까지 배열로 들고있고 지금 몇개 들어있는지 카운터로 관리
typedef struct
{
    Widget *items[MAX_WIDGETS]; // 위젯 포인터들의 배열 포인터를 담는 배열이지 위젯자체가 아님
    int count;                  // 현재 배열에 들어있는 위젯 개수
} Screen;                       //

/* ── 위젯 종류별 동작 ─────────────────────────────────────────── */
static void button_render(Widget *self) // 버튼을 화면에 출력하는 함수 셀프는 이 함수를 호출한 그 위젯 자신을 가르킴
{
    printf("  [Button #%d] \"%s\"\n", self->id, self->label);
}
static void label_render(Widget *self) // 라벨을 화면에 출력하는 함수
{
    printf("  Label #%d: %s\n", self->id, self->label);
}
static void dialog_render(Widget *self) // 다이얼로그를 화면에 출력하는 함수
{
    printf("  <<Dialog #%d>> %s\n", self->id, self->label);
}

static void widget_noop_event(Widget *self, int code) // 이벤트를 받아도 딱히 반응 안하는 위젯들을 공통으로 쓰는 함수
{
    (void)self; // 이 매개변수 안쓸건데 컴파일러보고 경고하지 말라고하는거
    (void)code;
}

/* 다이얼로그는 이벤트 코드 1(닫기)을 받으면 스스로 정리(파괴)된다 */
// 함수 몸통은 밑에있고 여기서는 이런 함수가 있다는 것만 미리 선언(전방선언)
// DIALOG_VT 이함수를 참조해야하는데 아직 함수 본문을 안만들었가때문
static void dialog_on_event(Widget *self, int code);
// 각 위젯 종류별로 그리기함수 이벤트 처리함수 세트를 미리 만들어 둔것 widget_new()에서 이 중 하나의 주소를 넘겨받아 위젯에 붙임
static const VTable BUTTON_VT = {button_render, widget_noop_event};
static const VTable LABEL_VT = {label_render, widget_noop_event};
static const VTable DIALOG_VT = {dialog_render, dialog_on_event};
// 새 위젯을 하나 만들어서 초기화 한뒤 그 포인터를 돌려주는 함수 //그럼 저기서 초기화를 하라는게 없는데 어떻게 저절로 초기화되는지?
static Widget *widget_new(const VTable *vt, int id, const char *label)
{

    /* [Thinking Point]
     *   w 에 아직 아무 값도 넣지 않았는데, sizeof *w 로 *w 를 써도 괜찮은 이유는?
     *   tip 1. sizeof 는 피연산자를 '실행(역참조)'하지 않고 '타입'만 본다.
     *          → *w 의 타입(Widget)만 필요할 뿐, w 를 실제로 따라가지 않는다.
     *   tip 2. 그래서 sizeof *w 는 (VLA 제외) 컴파일 타임에 sizeof(Widget) 상수로 치환된다.
     *   생각해보기: sizeof(Widget) 대신 sizeof *w 로 쓰면 어떤 장점이 있을까?
     */
    Widget *w = malloc(sizeof *w); // 위젯하나만큼 힙 메모리 할당
    if (!w)                        // w==NULL 이랑 같은의미 메모리 할당 실패시 실행
    {
        perror("malloc"); // 운영체제가 발생시킨 에러 이유를 찾아 화면에 출력함
        exit(1);          // 비정상 종료코드 1을 전달해 프로그램 즉시 끝냄
    }
    // 지금 이부분은 어떤 작업 하고있는거야? 저 문자열 복사랑 문자열 끝에 널문자 보장 저건 뭔뜻인데? 사이즈오브로 공간 재서 w->label이 뭔뜻인거지
    w->vtbl = vt; // 이 위젯의 종류를 결정(버튼/라벨/다디얼로그 vttable 중 하나->뭐 랜덤으로 결정된느거야?)
    w->id = id;
    w->closed = 0;                                  // 새로 만든 위젯은 당연히 아직 안닫힌 상태 왜 아직 안닫힌건데? 0으로 설정해서? 0으로 설정하면 닫혀서 위젯이 안보이니까? 꼭 위젯 닫혔는지 안닫혔는지 설정 해줘야하나?
    strncpy(w->label, label, sizeof(w->label) - 1); // 라벨 문자열 복사(버퍼 넘침 방지)
    w->label[sizeof(w->label) - 1] = '\0';          // 문자열 끝에 반드시 null문자 보장
    return w;
}
// 이거는 갑자가 또 왜 해제해?
static void widget_destroy(Widget *w) // 위젯이 차지하던 힙 메모리를 해제하는 함수(이 함수 호출 후 그 주소는 더이상 유효하지않음)
{
    free(w);
}

/* ── Screen ──────────────────────────────────────────────────── */
static void screen_add(Screen *s, Widget *w) // 스크린 배열에 위젯 포인터 하나를 추가하는 함수
{
    if (s->count < MAX_WIDGETS)   // 배열이 꽉 안찼으면
        s->items[s->count++] = w; // 맨뒤에 추가하고 카운트를 1늘림 근데 저 카운터++이쪽에 괄호는 왜치는거야?
}
// 코드라는 이벤트를 아이템 배열에있는 위젯들한테 순서대로 하나씩 전달해라 각 위젯이 그이벤트를 닫으면 알아서 반응해라
static void screen_dispatch(Screen *s, int code)
{
    for (int i = 0; i < s->count; i++)
    {
        Widget *w = s->items[i];
        if (!w)                     // 이 슬롯이 이미 프리되고 널 처리된 상태면
            continue;               // 아무것도 안하고 다름 슬롯으로 건너뜀 안하면 Null->vtbl 접근하다 죽음
        w->vtbl->on_event(w, code); // w->vtbl은 이 위젯의 이벤트 처리 함수 묶음이고 그중 on_event를실제로 호출 버튼/라벨이면 아무것도 안하는 함수가 다이얼로그면 다이얼로그 온 이벤트가 실행됨 같은 코드 한줄이 위젯 종류에 따라 다르게 동작하는 지점 이게 뭔말이야?? 함수는 그냥   w->vtbl->on_event(w, code)이거인데?
    }
}

static void screen_render(Screen *s) // 스크린에 담긴 위젯들을 순서대로 한 프레임 그리는 함수
{
    for (int i = 0; i < s->count; i++)
    {
        Widget *w = s->items[i];
        if (!w)             // 널 슬롯 이미 정리된 위젯자리는 건너뜀
            continue;       // Null 이면 건너뛰기
        w->vtbl->render(w); // w->vtbl 이 위젯 함수 묶음->그 묶음 안의 렌더 함수 포인터를 꺼냄->그 함수를 이 위젯 자신(w)로 넘겨서 호출 즉 이 위젯 종류에 맞는 그리기 함수를 이 위젯 데이터로 실행해라 라는 뜻
    }
}
// 스크린 안에있는 위젯들을 하나씩 순서대로 훑어 보면서 닫힌애가 있으면 처리한다
// 다이얼로그 전용 이벤드 핸들러 닫기 이벤트 코드==1을 받으면 닫힘 표시만 남김
static void dialog_on_event(Widget *self, int code)
{
    if (code == 1)
    {
        self->closed = 1; // 셀프는 스크린디스패치에서 넘겨준 w와 같은 주소를 가리킴(이름만 다를뿐 같은 위젯) 왜 같은 위젯인데? 여기서 직접 프리하지않는 이유 이 위젯은 자신이 스크린의 몇번째 슬롯인지 모르기때문 ->왜 모름? 프리는 슬롯을 관리하는 스크린쪽 메인 에서 처리하는게 맞음
    }
}
// 다이얼로그가 닫혔을때 보여줄 상태 메세지를 문자열을 힙에 새로 만드는 함수
static char *app_build_status(const char *text)
{
    char *msg = malloc(sizeof(Widget)); // 위젯하나 크기만큼 할당 원래 이 크기때문에 유즈애프터프리가 재현됐던것
    if (!msg)
        exit(1);

    /* [테스트용 연출] 재사용한 메모리를 0xAB 로 '일부러' 덮어써서 오염시킨다.
     * 실무라면 다른 기능이 우연히 이 자리를 덮어쓰겠지만, 여기서는 UAF 크래시를
     * 매번 똑같이(결정적으로) 재현하기 위해 인위적으로 채운다.
     * glibc(리눅스) 환경 (tcache)에서만 유효하다. 환경&상황에 따라 msg는 새로운 주소로 할당될 수 있다.
     */
    memset(msg, 0xAB, sizeof(Widget));
    snprintf(msg, sizeof(Widget), "STATUS: %s", text); // 실제로 쓸 메시지를 그 위에 덮어씀
    return msg;
}

int main(void)
{
    Screen s = {.count = 0}; // 빈화면 하나 생성 카운트를 0으로 명시적 초기화
    // 위젯 네개를 만들어서 화면에 추가 위젯뉴가 힙에 만든 포인터를 바로 스크린 add에 넘김
    screen_add(&s, widget_new(&LABEL_VT, 10, "Welcome"));
    screen_add(&s, widget_new(&BUTTON_VT, 11, "OK"));
    screen_add(&s, widget_new(&DIALOG_VT, 12, "Are you sure?")); /* items[2] */
    screen_add(&s, widget_new(&BUTTON_VT, 13, "Cancel"));

    printf("frame 1:\n");
    screen_render(&s);      // 1차 렌더링 위젯 4개 다 그려짐
    screen_dispatch(&s, 1); // 닫기 이벤트 코드==1 다이얼로그의 클로즈드가 1로 바뀜 아직 프리는 안됨

    /* TODO 닫힌(closed) 위젯을 여기서 정리(free + 해당 슬롯 NULL)할 필요가 있음 */
    for (int i = 0; i < s.count; i++) // 여기서 닫힌 위젯 정리를 직접함 프리하고 그 슬롯을 널로 만들어서
    {
        Widget *w = s.items[i]; // 이후에 아무도 이죽은 포인터를 실수로 다시 쓰지못하게 막음 i번째 슬롯의 위젯 포인터를 w로 꺼냄

        if (w->closed == 1) //  닫힘 표시가 된 위젯이면
        {
            free(s.items[i]);  // 메모리 해제
            s.items[i] = NULL; // 배열의 이자리를 비어있음으로 표시
        }
    }
    char *status = app_build_status("dialog closed"); // 다이얼로그가 프리된 바로 그 자리를 이 스태터스 문자열이 재사용할 가능성이 높음 재사용 문제 없지만 위에서 슬롯을 널 처리 했으니 이 주소를 위젯으로 착각해서 쓸일이 없음
    printf("%s\n", status);
    printf("frame 2:\n");
    screen_render(&s);

    free(status);
    for (int i = 0; i < s.count; i++)
        free(s.items[i]); // 이미 널 처리된 슬롯이 섞여있어도 프리 널은 안전하게 아무일도 안하므로 문제없음
    return 0;
}