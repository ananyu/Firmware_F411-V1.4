/*
    FreeRTOS V9.0.0 - Copyright (C) 2016 Real Time Engineers Ltd.
    All rights reserved

    VISIT http://www.FreeRTOS.org TO ENSURE YOU ARE USING THE LATEST VERSION.

    This file is part of the FreeRTOS distribution.

    FreeRTOS is free software; you can redistribute it and/or modify it under
    the terms of the GNU General Public License (version 2) as published by the
    Free Software Foundation >>>> AND MODIFIED BY <<<< the FreeRTOS exception.

    ***************************************************************************
    >>!   NOTE: The modification to the GPL is included to allow you to     !<<
    >>!   distribute a combined work that includes FreeRTOS without being   !<<
    >>!   obliged to provide the source code for proprietary components     !<<
    >>!   outside of the FreeRTOS kernel.                                   !<<
    ***************************************************************************

    FreeRTOS is distributed in the hope that it will be useful, but WITHOUT ANY
    WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
    FOR A PARTICULAR PURPOSE.  Full license text is available on the following
    link: http://www.freertos.org/a00114.html

    ***************************************************************************
     *                                                                       *
     *    FreeRTOS provides completely free yet professionally developed,    *
     *    robust, strictly quality controlled, supported, and cross          *
     *    platform software that is more than just the market leader, it     *
     *    is the industry's de facto standard.                               *
     *                                                                       *
     *    Help yourself get started quickly while simultaneously helping     *
     *    to support the FreeRTOS project by purchasing a FreeRTOS           *
     *    tutorial book, reference manual, or both:                          *
     *    http://www.FreeRTOS.org/Documentation                              *
     *                                                                       *
    ***************************************************************************

    http://www.FreeRTOS.org/FAQHelp.html - Having a problem?  Start by reading
    the FAQ page "My application does not run, what could be wrong?".  Have you
    defined configASSERT()?

    http://www.FreeRTOS.org/support - In return for receiving this top quality
    embedded software for free we request you assist our global community by
    participating in the support forum.

    http://www.FreeRTOS.org/training - Investing in training allows your team to
    be as productive as possible as early as possible.  Now you can receive
    FreeRTOS training directly from Richard Barry, CEO of Real Time Engineers
    Ltd, and the world's leading authority on the world's leading RTOS.

    http://www.FreeRTOS.org/plus - A selection of FreeRTOS ecosystem products,
    including FreeRTOS+Trace - an indispensable productivity tool, a DOS
    compatible FAT file system, and our tiny thread aware UDP/IP stack.

    http://www.FreeRTOS.org/labs - Where new FreeRTOS products go to incubate.
    Come and try FreeRTOS+TCP, our new open source TCP/IP stack for FreeRTOS.

    http://www.OpenRTOS.com - Real Time Engineers ltd. license FreeRTOS to High
    Integrity Systems ltd. to sell under the OpenRTOS brand.  Low cost OpenRTOS
    licenses offer ticketed support, indemnification and commercial middleware.

    http://www.SafeRTOS.com - High Integrity Systems also provide a safety
    engineered and independently SIL3 certified version for use in safety and
    mission critical applications that require provable dependability.

    1 tab == 4 spaces!
*/

/*
 * This is the list implementation used by the scheduler.  While it is tailored
 * heavily for the schedulers needs, it is also available for use by
 * application code.
 *
 * list_ts can only store pointers to list_item_ts.  Each ListItem_t contains a
 * numeric value (xItemValue).  Most of the time the lists are sorted in
 * descending item value order.
 *
 * Lists are created already containing one list item.  The value of this
 * item is the maximum possible that can be stored, it is therefore always at
 * the end of the list and acts as a marker.  The list member pxHead always
 * points to this marker - even though it is at the tail of the list.  This
 * is because the tail contains a wrap back pointer to the true head of
 * the list.
 *
 * In addition to it's value, each list item contains a pointer to the next
 * item in the list (pxNext), a pointer to the list it is in (pxContainer)
 * and a pointer to back to the object that contains it.  These later two
 * pointers are included for efficiency of list manipulation.  There is
 * effectively a two way link between the object containing the list item and
 * the list item itself.
 *
 * * 这是调度器所使用的链表实现。虽然它为调度器的需求做了大量定制，
 * 但同样可以供应用程序代码使用。
 *
 * 链表（list_t）只能存放指向链表项（ListItem_t）的指针。每个 ListItem_t
 * 都带有一个数值（xItemValue）。大多数情况下，链表按该项数值排序。
 *
 * 链表在创建时就自带一个链表项。它的值是可存储的最大值（portMAX_DELAY），
 * 因此它永远位于链表末尾，充当一个标记（即哨兵节点）。链表成员 pxHead
 * 始终指向这个标记——即使它在链表尾部。这是因为尾节点里存着一个
 * "回绕指针"，指向真正的链表头部。
 *
 * 除了自身的数值外，每个链表项还包含：
 *   - 指向链表中下一个项的指针（pxNext）
 *   - 指向它所属链表的指针（pxContainer）
 *   - 指回拥有它的那个对象的指针
 * 后两个指针是为了高效操作链表而设。实际上，"包含链表项的对象"
 * 与"链表项本身"之间构成了一条双向链路。
 * \page ListIntroduction List Implementation
 * \ingroup FreeRTOSIntro
 */

#ifndef INC_FREERTOS_H
	#error FreeRTOS.h must be included before list.h
#endif

#ifndef LIST_H
#define LIST_H

/*
 * The list structure members are modified from within interrupts, and therefore
 * by rights should be declared volatile.  However, they are only modified in a
 * functionally atomic way (within critical sections of with the scheduler
 * suspended) and are either passed by reference into a function or indexed via
 * a volatile variable.  Therefore, in all use cases tested so far, the volatile
 * qualifier can be omitted in order to provide a moderate performance
 * improvement without adversely affecting functional behaviour.  The assembly
 * instructions generated by the IAR, ARM and GCC compilers when the respective
 * compiler's options were set for maximum optimisation has been inspected and
 * deemed to be as intended.  That said, as compiler technology advances, and
 * especially if aggressive cross module optimisation is used (a use case that
 * has not been exercised to any great extend) then it is feasible that the
 * volatile qualifier will be needed for correct optimisation.  It is expected
 * that a compiler removing essential code because, without the volatile
 * qualifier on the list structure members and with aggressive cross module
 * optimisation, the compiler deemed the code unnecessary will result in
 * complete and obvious failure of the scheduler.  If this is ever experienced
 * then the volatile qualifier can be inserted in the relevant places within the
 * list structures by simply defining configLIST_VOLATILE to volatile in
 * FreeRTOSConfig.h (as per the example at the bottom of this comment block).
 * If configLIST_VOLATILE is not defined then the preprocessor directives below
 * will simply #define configLIST_VOLATILE away completely.
 *
 * To use volatile list structure members then add the following line to
 * FreeRTOSConfig.h (without the quotes):
 * "#define configLIST_VOLATILE volatile"
 *  * 链表结构体的成员会在中断内部被修改，因此按理说应该声明为 volatile。
 * 然而，它们的修改方式在功能上都是原子的（在临界区内，或在调度器
 * 挂起期间进行），并且要么以引用（指针）方式传给函数，要么通过一个
 * volatile 变量来索引访问。因此，在迄今为止测试过的所有使用场景中，
 * 省略 volatile 限定符可以带来适度的性能提升，而不会影响功能的正确性。
 * 我们已经检查了 IAR、ARM（Keil）和 GCC 三种编译器在各自开到最高
 * 优化等级时生成的汇编指令，确认其行为符合预期。
 *
 * 话虽如此，随着编译器技术的进步，尤其是当使用激进的跨模块优化时
 * （这种使用场景尚未被大量验证），volatile 限定符仍有可能是正确编译
 * 所必需的。可以预见的是：如果编译器因为在链表结构体成员上没有
 * volatile 限定符、且又开启了激进跨模块优化的情况下，认定某段代码
 * 无用而将其删除，其后果将是调度器彻底且明显地失效。
 *
 * 如果真的遇到这种情况，只需在 FreeRTOSConfig.h 中把 configLIST_VOLATILE
 * 定义为 volatile，即可在链表结构体的相关位置插入 volatile 限定符
 * （具体做法见本注释块末尾的示例）。
 * 如果没有定义 configLIST_VOLATILE，下面的预处理指令就会把它
 * 定义为空，完全等效于没加 volatile。
 *
 * 若想使用 volatile 修饰链表结构体成员，只需在 FreeRTOSConfig.h 中
 * 加入下面这一行（不带引号）：
 * "#define configLIST_VOLATILE volatile"
 */
#ifndef configLIST_VOLATILE
	#define configLIST_VOLATILE
#endif /* configSUPPORT_CROSS_MODULE_OPTIMISATION */

#ifdef __cplusplus
extern "C" {
#endif

/* Macros that can be used to place known values within the list structures,
then check that the known values do not get corrupted during the execution of
the application.   These may catch the list data structures being overwritten in
memory.  They will not catch data errors caused by incorrect configuration or
use of FreeRTOS.
 这些宏用于在链表结构体中预先放置已知的值（"检查字节"），
然后在应用程序运行过程中检查这些已知值是否被破坏。
这可以捕获链表数据结构被内存越界覆盖的情况。
但它们无法捕获由 FreeRTOS 配置错误或使用不当造成的数据错误。
*/
#if( configUSE_LIST_DATA_INTEGRITY_CHECK_BYTES == 0 )
	/* Define the macros to do nothing. */
	#define listFIRST_LIST_ITEM_INTEGRITY_CHECK_VALUE
	#define listSECOND_LIST_ITEM_INTEGRITY_CHECK_VALUE
	#define listFIRST_LIST_INTEGRITY_CHECK_VALUE
	#define listSECOND_LIST_INTEGRITY_CHECK_VALUE
	#define listSET_FIRST_LIST_ITEM_INTEGRITY_CHECK_VALUE( pxItem )
	#define listSET_SECOND_LIST_ITEM_INTEGRITY_CHECK_VALUE( pxItem )
	#define listSET_LIST_INTEGRITY_CHECK_1_VALUE( pxList )
	#define listSET_LIST_INTEGRITY_CHECK_2_VALUE( pxList )
	#define listTEST_LIST_ITEM_INTEGRITY( pxItem )
	#define listTEST_LIST_INTEGRITY( pxList )
#else
	/* Define macros that add new members into the list structures. */
	#define listFIRST_LIST_ITEM_INTEGRITY_CHECK_VALUE				TickType_t xListItemIntegrityValue1;
	#define listSECOND_LIST_ITEM_INTEGRITY_CHECK_VALUE				TickType_t xListItemIntegrityValue2;
	#define listFIRST_LIST_INTEGRITY_CHECK_VALUE					TickType_t xListIntegrityValue1;
	#define listSECOND_LIST_INTEGRITY_CHECK_VALUE					TickType_t xListIntegrityValue2;

	/* Define macros that set the new structure members to known values. */
	#define listSET_FIRST_LIST_ITEM_INTEGRITY_CHECK_VALUE( pxItem )		( pxItem )->xListItemIntegrityValue1 = pdINTEGRITY_CHECK_VALUE
	#define listSET_SECOND_LIST_ITEM_INTEGRITY_CHECK_VALUE( pxItem )	( pxItem )->xListItemIntegrityValue2 = pdINTEGRITY_CHECK_VALUE
	#define listSET_LIST_INTEGRITY_CHECK_1_VALUE( pxList )		( pxList )->xListIntegrityValue1 = pdINTEGRITY_CHECK_VALUE
	#define listSET_LIST_INTEGRITY_CHECK_2_VALUE( pxList )		( pxList )->xListIntegrityValue2 = pdINTEGRITY_CHECK_VALUE

	/* Define macros that will assert if one of the structure members does not
	contain its expected value. */
	#define listTEST_LIST_ITEM_INTEGRITY( pxItem )		configASSERT( ( ( pxItem )->xListItemIntegrityValue1 == pdINTEGRITY_CHECK_VALUE ) && ( ( pxItem )->xListItemIntegrityValue2 == pdINTEGRITY_CHECK_VALUE ) )
	#define listTEST_LIST_INTEGRITY( pxList )			configASSERT( ( ( pxList )->xListIntegrityValue1 == pdINTEGRITY_CHECK_VALUE ) && ( ( pxList )->xListIntegrityValue2 == pdINTEGRITY_CHECK_VALUE ) )
#endif /* configUSE_LIST_DATA_INTEGRITY_CHECK_BYTES */


/*
 * Definition of the only type of object that a list can contain.
 */
struct xLIST_ITEM
{
	listFIRST_LIST_ITEM_INTEGRITY_CHECK_VALUE			/*< Set to a known value if configUSE_LIST_DATA_INTEGRITY_CHECK_BYTES is set to 1. */
	configLIST_VOLATILE TickType_t xItemValue;			/*< The value being listed.  In most cases this is used to sort the list in descending order. */
	struct xLIST_ITEM * configLIST_VOLATILE pxNext;		/*< Pointer to the next ListItem_t in the list. */
	struct xLIST_ITEM * configLIST_VOLATILE pxPrevious;	/*< Pointer to the previous ListItem_t in the list. */
	void * pvOwner;										/*< Pointer to the object (normally a TCB) that contains the list item.  There is therefore a two way link between the object containing the list item and the list item itself. */
	void * configLIST_VOLATILE pvContainer;				/*< Pointer to the list in which this list item is placed (if any). */
	listSECOND_LIST_ITEM_INTEGRITY_CHECK_VALUE			/*< Set to a known value if configUSE_LIST_DATA_INTEGRITY_CHECK_BYTES is set to 1. */
};
typedef struct xLIST_ITEM ListItem_t;					/* For some reason lint wants this as two separate definitions. */

struct xMINI_LIST_ITEM
{
	listFIRST_LIST_ITEM_INTEGRITY_CHECK_VALUE			/*< Set to a known value if configUSE_LIST_DATA_INTEGRITY_CHECK_BYTES is set to 1. */
	configLIST_VOLATILE TickType_t xItemValue;
	struct xLIST_ITEM * configLIST_VOLATILE pxNext;
	struct xLIST_ITEM * configLIST_VOLATILE pxPrevious;
};
typedef struct xMINI_LIST_ITEM MiniListItem_t;

/*
 * Definition of the type of queue used by the scheduler.
 */
typedef struct xLIST
{
	listFIRST_LIST_INTEGRITY_CHECK_VALUE				/*< Set to a known value if configUSE_LIST_DATA_INTEGRITY_CHECK_BYTES is set to 1. */
	configLIST_VOLATILE UBaseType_t uxNumberOfItems;
	ListItem_t * configLIST_VOLATILE pxIndex;			/*< Used to walk through the list.  Points to the last item returned by a call to listGET_OWNER_OF_NEXT_ENTRY (). */
	MiniListItem_t xListEnd;							/*< List item that contains the maximum possible item value meaning it is always at the end of the list and is therefore used as a marker. */
	listSECOND_LIST_INTEGRITY_CHECK_VALUE				/*< Set to a known value if configUSE_LIST_DATA_INTEGRITY_CHECK_BYTES is set to 1. */
} List_t;

/*
 * Access macro to set the owner of a list item.  The owner of a list item
 * is the object (usually a TCB) that contains the list item.
 *
 * \page listSET_LIST_ITEM_OWNER listSET_LIST_ITEM_OWNER
 * \ingroup LinkedList
 */
#define listSET_LIST_ITEM_OWNER( pxListItem, pxOwner )		( ( pxListItem )->pvOwner = ( void * ) ( pxOwner ) )

/*
 * Access macro to get the owner of a list item.  The owner of a list item
 * is the object (usually a TCB) that contains the list item.
 *
 * \page listSET_LIST_ITEM_OWNER listSET_LIST_ITEM_OWNER
 * \ingroup LinkedList
 */
#define listGET_LIST_ITEM_OWNER( pxListItem )	( ( pxListItem )->pvOwner )

/*
 * Access macro to set the value of the list item.  In most cases the value is
 * used to sort the list in descending order.
 *
 * \page listSET_LIST_ITEM_VALUE listSET_LIST_ITEM_VALUE
 * \ingroup LinkedList
 */
#define listSET_LIST_ITEM_VALUE( pxListItem, xValue )	( ( pxListItem )->xItemValue = ( xValue ) )

/*
 * Access macro to retrieve the value of the list item.  The value can
 * represent anything - for example the priority of a task, or the time at
 * which a task should be unblocked.
 *
 * \page listGET_LIST_ITEM_VALUE listGET_LIST_ITEM_VALUE
 * \ingroup LinkedList
 */
#define listGET_LIST_ITEM_VALUE( pxListItem )	( ( pxListItem )->xItemValue )

/*
 * Access macro to retrieve the value of the list item at the head of a given
 * list.
 *
 * \page listGET_LIST_ITEM_VALUE listGET_LIST_ITEM_VALUE
 * \ingroup LinkedList
 */
#define listGET_ITEM_VALUE_OF_HEAD_ENTRY( pxList )	( ( ( pxList )->xListEnd ).pxNext->xItemValue )

/*
 * Return the list item at the head of the list.
 *
 * \page listGET_HEAD_ENTRY listGET_HEAD_ENTRY
 * \ingroup LinkedList
 */
#define listGET_HEAD_ENTRY( pxList )	( ( ( pxList )->xListEnd ).pxNext )

/*
 * Return the list item at the head of the list.
 *
 * \page listGET_NEXT listGET_NEXT
 * \ingroup LinkedList
 */
#define listGET_NEXT( pxListItem )	( ( pxListItem )->pxNext )

/*
 * Return the list item that marks the end of the list
 *
 * \page listGET_END_MARKER listGET_END_MARKER
 * \ingroup LinkedList
 */
#define listGET_END_MARKER( pxList )	( ( ListItem_t const * ) ( &( ( pxList )->xListEnd ) ) )

/*
 * Access macro to determine if a list contains any items.  The macro will
 * only have the value true if the list is empty.
 *
 * \page listLIST_IS_EMPTY listLIST_IS_EMPTY
 * \ingroup LinkedList
 */
#define listLIST_IS_EMPTY( pxList )	( ( BaseType_t ) ( ( pxList )->uxNumberOfItems == ( UBaseType_t ) 0 ) )

/*
 * Access macro to return the number of items in the list.
 */
#define listCURRENT_LIST_LENGTH( pxList )	( ( pxList )->uxNumberOfItems )

/*
 * Access function to obtain the owner of the next entry in a list.
 *
 * The list member pxIndex is used to walk through a list.  Calling
 * listGET_OWNER_OF_NEXT_ENTRY increments pxIndex to the next item in the list
 * and returns that entry's pxOwner parameter.  Using multiple calls to this
 * function it is therefore possible to move through every item contained in
 * a list.
 *
 * The pxOwner parameter of a list item is a pointer to the object that owns
 * the list item.  In the scheduler this is normally a task control block.
 * The pxOwner parameter effectively creates a two way link between the list
 * item and its owner.
 *
 * @param pxTCB pxTCB is set to the address of the owner of the next list item.
 * @param pxList The list from which the next item owner is to be returned.
 *
 * \page listGET_OWNER_OF_NEXT_ENTRY listGET_OWNER_OF_NEXT_ENTRY
 * \ingroup LinkedList
 *  * 用于获取链表中"下一个节点"所有者的访问函数（宏）。
 *
 * 链表成员 pxIndex 用来在链表中行走。调用 listGET_OWNER_OF_NEXT_ENTRY
 * 会把 pxIndex 递增到链表中的下一个节点，并返回该节点的 pvOwner。
 * 因此，多次调用这个宏，就可以走遍链表中包含的每一个节点。
 *
 * 链表项的 pvOwner 参数是指向"拥有这个链表项的对象"的指针。
 * 在调度器中，这通常就是一个任务控制块（TCB）。
 * pvOwner 实际上在"链表项"与"它的所有者"之间建立了一条双向链路。
 *
 * @param pxTCB   输出参数：被设置为下一个链表项所有者的地址。
 * @param pxList  从这条链表中取下一个节点的所有者。
 *
 * \page listGET_OWNER_OF_NEXT_ENTRY listGET_OWNER_OF_NEXT_ENTRY
 * \ingroup LinkedList
 */
#define listGET_OWNER_OF_NEXT_ENTRY( pxTCB, pxList )										\
{																							\
    List_t * const pxConstList = ( pxList );													\
	/* Increment the index to the next item and return the item, ensuring */				\
	/* we don't return the marker used at the end of the list.  */							\
	( pxConstList )->pxIndex = ( pxConstList )->pxIndex->pxNext;							\
	if( ( void * ) ( pxConstList )->pxIndex == ( void * ) &( ( pxConstList )->xListEnd ) )	\
	{																						\
		( pxConstList )->pxIndex = ( pxConstList )->pxIndex->pxNext;						\
	}																						\
	( pxTCB ) = ( pxConstList )->pxIndex->pvOwner;											\
}


/*
 * Access function to obtain the owner of the first entry in a list.  Lists
 * are normally sorted in ascending item value order.
 *
 * This function returns the pxOwner member of the first item in the list.
 * The pxOwner parameter of a list item is a pointer to the object that owns
 * the list item.  In the scheduler this is normally a task control block.
 * The pxOwner parameter effectively creates a two way link between the list
 * item and its owner.
 *
 * @param pxList The list from which the owner of the head item is to be
 * returned.
 *
 * \page listGET_OWNER_OF_HEAD_ENTRY listGET_OWNER_OF_HEAD_ENTRY
 * \ingroup LinkedList
 *  * 用于获取链表中"第一个节点"的所有者的访问函数（宏）。
 * 链表通常按节点的数值升序排列。
 *
 * 这个宏返回链表中第一个节点的 pvOwner 成员。
 * 链表项的 pvOwner 是指向"拥有该链表项的对象"的指针。
 * 在调度器中，这通常就是一个任务控制块（TCB）。
 * pvOwner 实际上在"链表项"与"它的所有者"之间建立了一条双向链路。
 *
 * @param pxList  要从中取出队头节点所有者的链表。
 */
#define listGET_OWNER_OF_HEAD_ENTRY( pxList )  ( (&( ( pxList )->xListEnd ))->pxNext->pvOwner )

/*
 * Check to see if a list item is within a list.  The list item maintains a
 * "container" pointer that points to the list it is in.  All this macro does
 * is check to see if the container and the list match.
 *
 * @param pxList The list we want to know if the list item is within.
 * @param pxListItem The list item we want to know if is in the list.
 * @return pdTRUE if the list item is in the list, otherwise pdFALSE.
 *  * 检查某个链表项是否位于某条链表中。链表项内部维护着一个
 * "容器"指针（pvContainer），指向它当前所在的链表。
 * 这个宏所做的全部事情，就是比较容器指针和给定的链表是否一致。
 *
 * @param pxList     要确认"链表项是否在其中"的那条链表。
 * @param pxListItem 要确认"是否在这条链表里"的那个链表项。
 * @return 如果链表项在该链表中，返回 pdTRUE，否则返回 pdFALSE。
 */
#define listIS_CONTAINED_WITHIN( pxList, pxListItem ) ( ( BaseType_t ) ( ( pxListItem )->pvContainer == ( void * ) ( pxList ) ) )

/*
 * Return the list a list item is contained within (referenced from).
 *
 * @param pxListItem The list item being queried.
 * @return A pointer to the List_t object that references the pxListItem
 */
#define listLIST_ITEM_CONTAINER( pxListItem ) ( ( pxListItem )->pvContainer )

/*
 * This provides a crude means of knowing if a list has been initialised, as
 * pxList->xListEnd.xItemValue is set to portMAX_DELAY by the vListInitialise()
 * function.
 * 这提供了一种比较"粗糙"的手段来判断链表是否已被初始化——
 * 因为 vListInitialise() 函数会把 pxList->xListEnd.xItemValue 设置为 portMAX_DELAY。

 */
#define listLIST_IS_INITIALISED( pxList ) ( ( pxList )->xListEnd.xItemValue == portMAX_DELAY )

/*
 * Must be called before a list is used!  This initialises all the members
 * of the list structure and inserts the xListEnd item into the list as a
 * marker to the back of the list.
 *
 * @param pxList Pointer to the list being initialised.
 * * 在使用链表之前必须调用！它初始化链表结构体的所有成员，
 * 并把 xListEnd 节点插入链表，作为链表尾部的标记（哨兵）。
 *
 * @param pxList  指向待初始化链表的指针。
 * \page vListInitialise vListInitialise
 * \ingroup LinkedList
 */
void vListInitialise( List_t * const pxList ) PRIVILEGED_FUNCTION;

/*
 * Must be called before a list item is used.  This sets the list container to
 * null so the item does not think that it is already contained in a list.
 *
 * @param pxItem Pointer to the list item being initialised.
 *
 * \page vListInitialiseItem vListInitialiseItem
 * \ingroup LinkedList
 */
void vListInitialiseItem( ListItem_t * const pxItem ) PRIVILEGED_FUNCTION;

/*
 * Insert a list item into a list.  The item will be inserted into the list in
 * a position determined by its item value (descending item value order).
 *
 * @param pxList The list into which the item is to be inserted.
 *
 * @param pxNewListItem The item that is to be placed in the list.
 *
 * \page vListInsert vListInsert
 * \ingroup LinkedList
 */
void vListInsert( List_t * const pxList, ListItem_t * const pxNewListItem ) PRIVILEGED_FUNCTION;

/*
 * Insert a list item into a list.  The item will be inserted in a position
 * such that it will be the last item within the list returned by multiple
 * calls to listGET_OWNER_OF_NEXT_ENTRY.
 *
 * The list member pxIndex is used to walk through a list.  Calling
 * listGET_OWNER_OF_NEXT_ENTRY increments pxIndex to the next item in the list.
 * Placing an item in a list using vListInsertEnd effectively places the item
 * in the list position pointed to by pxIndex.  This means that every other
 * item within the list will be returned by listGET_OWNER_OF_NEXT_ENTRY before
 * the pxIndex parameter again points to the item being inserted.
 *
 * @param pxList The list into which the item is to be inserted.
 *
 * @param pxNewListItem The list item to be inserted into the list.
 * * 将一个链表项插入链表。插入位置经过精心安排：
 * 在反复调用 listGET_OWNER_OF_NEXT_ENTRY 进行轮转时，
 * 新节点将是这条链上"最后被轮到"的那个。
 *
 * 链表成员 pxIndex 用于在链表中行走。调用 listGET_OWNER_OF_NEXT_ENTRY
 * 会把 pxIndex 递增到链表中的下一个节点。用 vListInsertEnd 插入节点，
 * 实际上是把节点放到 pxIndex 当前所指位置的"前面"（即上一站）。
 * 这意味着：轮转时，链表中其他所有节点都会先被
 * listGET_OWNER_OF_NEXT_ENTRY 返回，之后才轮到新插入的节点。
 *
 * @param pxList         要插入节点的那条链表。
 *
 * @param pxNewListItem  待插入的链表项。
 * \page vListInsertEnd vListInsertEnd
 * \ingroup LinkedList
 */
void vListInsertEnd( List_t * const pxList, ListItem_t * const pxNewListItem ) PRIVILEGED_FUNCTION;

/*
 * Remove an item from a list.  The list item has a pointer to the list that
 * it is in, so only the list item need be passed into the function.
 *
 * @param uxListRemove The item to be removed.  The item will remove itself from
 * the list pointed to by it's pxContainer parameter.
 *
 * @return The number of items that remain in the list after the list item has
 * been removed.
 *
 * \page uxListRemove uxListRemove
 * \ingroup LinkedList
 */
UBaseType_t uxListRemove( ListItem_t * const pxItemToRemove ) PRIVILEGED_FUNCTION;

#ifdef __cplusplus
}
#endif

#endif

