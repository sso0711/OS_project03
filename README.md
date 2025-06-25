# 1. Copy-on-Write Fork

기존의 fork()는 즉시 부모의 메모리를 복사하기 때문에 exec()까지의 과정에서 메모리를 비효율적으로 사용하게 된다.

따라서  다음과 같은 CoW(Copy-on-Write)를 구현해야 한다.

- 부모와 자식이 페이지를 공유한다.
- 자식이 수정되면 페이지를 복사한다.

Design

fork 과정에서 uvmcopy를 통해 부모 프로세스의 페이지테이블과 물리 메모리를 자식 프로세스에게 똑같이 복사하는데, 이 함수를 수정해 복사가 아닌 페이지를 공유하도록 해야했다.

처음 고민과 의문이 들었던 점과 해결과정은 다음과 같았다.

1. 쓰기 시도를 할 때 page fault가 남으로써 새 페이지로 복사가 이루어져야 하는데, 어떻게 page fault로 인식시킬 것인가?
    - 쓰기 시도 중 page fault가 발생하면 scause레지스터에 15가 담긴다는 것을 찾아내었다. 따라서 usertrap함수에서 r_cause()==15인 경우의 분기를 추가하여 핸들러를 구현하기로했다.
2. flag의 RSW bit를 CoW bit로 사용한다고 했는데, 이 bit의 용도는 무엇인가?
    - 이는 1번에서부터 이어지는데, r_cause()가 15인 경우는 cow page fault 뿐만 아니라 쓰기 권한을 위배한 일반적인 상황에서도 발생할 수 있다. 이를 어떻게 구분할지 고민하는 과정에서, 바로 이 CoW bit로 구분하는 것임을 깨달아 1번과 2번 문제가 같이 해결되었다.
3. page마다 reference count를 관리해야 하는데 어떤 자료구조로 어디에 구현해야 하는가



기존 `uvmcopy` 함수의 동작 과정을 분석해보았다.

![image](https://github.com/user-attachments/assets/25d50bee-423d-4b5f-b15f-5b21a8a93ce9)

이를 바탕으로 uvmcopy의 구현 계획을 작성해보면

1. 335번줄까지 가상주소에 해당하는 물리주소는 그대로 찾고, 메모리 복사는 하지 않을 것이므로 

이후 kalloc, memmove 과정을 없앤다.

2. flag에서 write 권한을 해제하고 CoW 비트를 설정한다.
3. 340번줄에서 mappages 함수를 수정해 자식 pagetable new의 가상주소 i에 부모의 물리페이지 pa를 매핑한다.
4. pa페이지의 reference count가 1 증가해야 한다.

그리고 쓰기가 발생되면 write권한이 없으므로 usertrap함수가 실행되고,

`usertrap`에서 r_cause()==15인 경우의 분기를 추가하여

- rsw비트를 통해 cow page fault인지 확인
    - 새 페이지 할당 (`kalloc`)
        - 
    - copyout함수를 통해 새 페이지에 내용 복사

![image](https://github.com/user-attachments/assets/4db3777b-ce8a-4a73-acab-8b960cec3ed1)


![image](https://github.com/user-attachments/assets/8b53d82b-a89f-4548-8f50-62a6d84da9ae)


riscv.h에 cow bit로 사용할 rsw 플래그 정의

기존 `copyout`  함수 동작 과정을 이해해 보았다.

![image](https://github.com/user-attachments/assets/f690ae63-40d6-475f-8a1b-c22fa344ec9c)


![image](https://github.com/user-attachments/assets/08cffb94-3e03-48b9-9e27-8d08b9d3753c)


다음과 같이 수정한다.

- cow page fault handling -  (copyout이 usertrap에 의해 호출된 경우)
    - 부모페이지와 공유 해제하므로 기존 pte의 pa를 찾고 count 1 감소 `sub_count(pa)`
    - memmove 함수를 통해 내용 복사
    
- else문에는 기존의 copyout 함수를 그대로 구현

이 때 위와 같이 copyout이 cow page fault에 의해 호출되어 cow logic을 수행하려는 것인지,

원래의 함수 로직을 따라야하는지를 구분하기 위한 방법이 고민이 되었다. 다음과 같은 두 가지 방법을 생각해보았다.

1.copyout함수가 어떤 경로로 호출되었는지를 나타내는 전역변수 사용

2.copyout함수는 본래 사용자 가상 주소인 dstva로 복사하는 함수이므로, dstva가 사용자 주소인지 커널 주소인지에 따라 나눌 수 있다.

1번의 경우 전역변수를 설정하는 과정에서 context switching이 일어날 수 있다는 점과,

원래 값으로 되돌려야하는 적절한 시점을 고려해야 한다.

따라서 더 간단해보이는 2번 방법을 사용하였다.

❗수정 

- copyout에 cow page fault임을 나타내는 int변수를 추가하였다.(0-기존copyout, 1-cow page fault)
    - 시스템에서 copyout이 사용되는 곳에 변경된 구조에 따라 파라미터 0 추가

Implementation

- 디자인 로직에 따라 구현했으나 수정된 부분이 있을 수 있습니다.
- 코드에 주석을 달아놓았습니다.

처음엔 kalloc, kfree 함수 내부에서 count를 조정하도록 구현하였다. 그런데 count가 변하는 상황은 아래와 같은 경우가 더 있다는 것을 깨닫고 count 증가 / count 감소하는 함수를 따로 만들었다.

- `add_count` uvmcopy과정에서 자식이 부모의 페이지를 공유하며 count 증가
- `sub_count` write가 발생하면 새 페이지를 할당하고 (kalloc 내부에서 count 변화)
    
    부모페이지의 count는 1 감소
    

Results

![image](https://github.com/user-attachments/assets/b72e6048-a64d-4c38-b375-adcd77882342)


pipe() 시스템 콜이 실패하는 원인은, 

제가 copyout 함수에 5번째 인자를 추가하여 구현을 했는데,  cow가 아닌 경우 원래 함수 로직으로 분기하도록 하였으나, 콜스택이 변경되어 호환이 잘 되지 않은 것이 아닌가 싶습니다.

Trouble Shooting

1.

![image](https://github.com/user-attachments/assets/f48e13c7-9075-4ae1-9cda-0d2d758bfede)


기존의 pte에 새로 할당한 물리 페이지를 매핑할 목적으로 mappages 함수를 사용했으나 이는 새로운 엔트리를 등록하는 함수로, 

mappages를 사용하면 안되었고 pte만 수정해주었다.

2. copyout 함수 오류
    - dstva는 void* 타입으로 변경, kalloc이 반환한 포인터를 대입
    - src는 pte_t* 타입

copyout 함수를 수정하는 과정에서 위와 같이 매개변수의 타입을 수정했다.

copyout함수를 다른 용도로 사용할 일이 없다고 생각하여 편의를 위해 바꾸었는데, 사실 이 함수는 wait함수에서도 사용되기 때문에 함부로 변경하면 안되었다.

(잘못된 copyout 함수 구현 사진)

![image](https://github.com/user-attachments/assets/a9bfdeb8-70f8-4584-bd88-b7ccdbc327b4)


Design에 작성한 내용은 이를 수정한 계획이다.

3. 부팅 실패

![image](https://github.com/user-attachments/assets/29f45136-146d-4a7a-b558-0dfedfcb7f7f)


- 원인 : initprocess fork 과정에서 오류가 났으므로 kinit
- 해결 : 초기 kinit, freeranges 함수 수정

4.

![image](https://github.com/user-attachments/assets/f9d5bce8-b797-4cfd-a8d1-98d1e2ff52cd)


- 해결 : copyout 함수에 아래와 같이 새로 할당받은 페이지의 권한 설정을 추가하고,
- 
  ```c
// 새로 할당받은 페이지에는 쓰기 가능, COW 비트는 해제
    *pte = PA2PTE((uint64)src) | (PTE_FLAGS(*pte) & ~PTE_RSW) | PTE_W | PTE_V | PTE_U;

    sfence_vma();   // TLB flush
  ```
    
    직접적인 원인은 아니겠지만 page table 매핑 정보가 바뀌었기 때문에 혹시몰라 TLB flush도 추가해주었더니 cowtest의 main함수가 실행 되고 5번 상태가 되었다.
    

```jsx
// 새로 할당받은 페이지에는 쓰기 가능, COW 비트는 해제
    *pte = PA2PTE((uint64)src) | (PTE_FLAGS(*pte) & ~PTE_RSW) | PTE_W | PTE_V | PTE_U;

    sfence_vma();   // TLB flush
```

5. 

![image](https://github.com/user-attachments/assets/7501092d-a69a-4d2b-8484-57bfda4c386f)


- 원인 : pid3은 cowtest 프로그램이다. cowtest의 main함수 시작 부분에 “COW TESTS STARTED”를 출력하도록 했는데, 이후 write를 시도 후 page fault 핸들링 과정에서 오류

# 2. Doubly-indirect blocks

기존 xv6는 12 direct blocks와 1 single indirect

이를 11 direct + 1 single indirect + 1 double indirect로 변경

Design & Implementation

![image](https://github.com/user-attachments/assets/9109ba06-43e7-4da4-9991-f2c60aba1a68)


![image](https://github.com/user-attachments/assets/f599f7a0-9ed7-4361-9b1b-30db64d7605e)


`file.h` 

![image](https://github.com/user-attachments/assets/42786147-84f4-4243-b466-5c0127a67d56)


`fs.h` 

![image](https://github.com/user-attachments/assets/278c21a7-b390-4ef0-a523-31b484551617)


`fs.c`

bmap, itrunc은 기존 함수 로직과 비슷하게 가되, double indirect block의 경우 분기를 추가하였다.

Results

![image](https://github.com/user-attachments/assets/e6024aff-70a4-44c9-8624-132734ea7800)


Trouble Shooting

1. FSSIZE 

![image](https://github.com/user-attachments/assets/dfa94656-ab56-40c9-b25e-05d954396745)


- 잘못된 인덱스 접근 오류로 생각하고, 구현 코드만 계속 뜯어보며 원인을 찾았는데, 사실 FSSIZE를 200000이 아닌 20000으로 잘못 설정한 것이 원인이었다. 내가 작성한 코드에 확신을 가지지 못했기 때문이었다.

# 3. Symbolic links

현재 xv6에서는 hard link만 지원하여 파일 시스템 간 참조, 링크가 불가능하다.

경로명(pathname) 기반의 참조, 

Design

`sys_open` 함수의 과정을 분석해보았다.

1. 새 파일을 생성
2. 또는 path에 해당하는 inode를 검색하고 inode 잠금
3. ip→type, ip→major, omode 등에 따라 처리
4. 파일 구조체, 파일 디스크립터 할당

파일 구조체와 파일 디스크립터를 할당하기 전에 symlink가 가리키는 파일을 찾아야 하므로

3번 4번 사이에 아래 과정을 추가하기로 했다.

3-1. p→type이 T_SYMLINK 이면

- O_NOFOLLOW 플래그가 있으면
    - 해당 링크를 연다.
- O_NOFOLLOW 플래그가 없으면
    - 재귀적으로 최대 깊이 10까지 링크를 따라간다.
    - cycle detect
    - broken link

`sys_symlink`

create함수를 수정하지 않기 위해 create함수의 로직을 포함시켜 작성한다.

Implementation

![image](https://github.com/user-attachments/assets/bf55750f-8111-4e17-b725-77cbc212adca)

T_SYMLINK 파일 타입을 추가합니다.


![image](https://github.com/user-attachments/assets/e873ea68-8386-4eb7-8ad3-7fe72a37205c)

O_NOFOLLOW 플래그를 추가하고 다른 플래그들과 겹치지 않는 값으로 설정합니다.

Results

![image](https://github.com/user-attachments/assets/94dd12fd-af28-4fc5-ab60-6873199ebfa8)


Trouble Shooting

1. sys_open : 
    
    ![image.png](attachment:1251b347-7b59-41cb-a76a-1ee5100b9123:image.png)
    
- 원인 : 불필요하게 while 루프를
- 해결 : 먼저

![image.png](attachment:9eed85ea-efdd-4fd7-bc51-da6ffedf0716:image.png)

타겟파일은 T_SYMLINK 타입이 아니므로 찾은경우 탐색 종료





2. sys_open

![image](https://github.com/user-attachments/assets/17d879e9-8bd7-4650-b2ad-22e24e437976)


- 원인 : cycle이 발생하면 재귀 깊이가 10이 넘어버리므로 depth변수만으로 cycle detect를 할 수 있을 것이라 생각했지만 깊이 10 미만일 때 cycle이 발생하는 경우를 간과했다.
- 해결 : ip를 담는 배열을 추가
    
    ![image](https://github.com/user-attachments/assets/c75815c7-8ca2-4327-aee8-de577ebaad7f)

    
    - xv6에는 page의 메타데이터를 관리하는 자료구조가 따로 없으므로, 전체 물리 페이지 갯수 크기의 reference  count배열을 따로 만들었다.
    - 물리 주소에 대응하는 페이지 번호(refcount배열의 인덱스)를 구하는 과정이 필요하다.
    - 특정 페이지의 count가 변경되는 와중에 context switch가 발생하면 안되므로, kmem.lock을 먼저 획득해야 한다.
