#include <stdlib.h>

#include "glist.h"

/*释放该链表的空间 */
void
g_list_free (GList *list)
{
	GList *l = list;

	while(l) {
		GList *tmp = l->next;
		free(l);
		l = tmp;
	}
}

/*获取链表中最后一个元素 */
GList*
g_list_last (GList *list)
{
  if (list)
    {
      while (list->next)
	  	list = list->next;
    }
  
  return list;
}

/*创建一个新节点，并将节点加入链表尾部，如果链表不存在，将该节点视为新链表头部 */
GList*
g_list_append (GList *list, gpointer data)
{
  GList *new_list;
  GList *last;
  
  new_list = malloc(sizeof(*new_list));
  new_list->data = data;
  new_list->next = NULL;
  
  if (list)
    {
      last = g_list_last (list);
      last->next = new_list;
      new_list->prev = last;

      return list;
    }
  else
    {
      new_list->prev = NULL;
      return new_list;
    }
}

/*将一个元素从链表中移除*/
static inline GList*
_g_list_remove_link (GList *list,
		     GList *link)
{
  if (link)
    {
      if (link->prev)
	link->prev->next = link->next;
      if (link->next)
	link->next->prev = link->prev;
      
      if (link == list)
	list = list->next;
      
      link->next = NULL;
      link->prev = NULL;
    }
  
  return list;
}

/*将元素从链表中移除并释放其空间 */
GList*
g_list_delete_link (GList *list,
		    GList *link_)
{
  list = _g_list_remove_link (list, link_);
  free (link_);

  return list;
}

/*获取链表的第一个元素 */
GList*
g_list_first (GList *list)
{
  if (list)
    {
      while (list->prev)
		list = list->prev;
    }
  
  return list;
}

/*将两个链表合并，并按照特定的比较方式排序*/
static GList *
g_list_sort_merge (GList     *l1, 
		   GList     *l2,
		   GFunc     compare_func,
		   gpointer  user_data)
{
  GList list, *l, *lprev;
  gint cmp;

  l = &list; 
  lprev = NULL;

  while (l1 && l2)
    {
      cmp = ((GCompareDataFunc) compare_func) (l1->data, l2->data, user_data);

      if (cmp <= 0)
      {
	 	 l->next = l1;
	 	 l1 = l1->next;
      } 
      else 
	  {
	 	 l->next = l2;
	 	 l2 = l2->next;
      }
      l = l->next;
      l->prev = lprev; 
      lprev = l;
    }
  l->next = l1 ? l1 : l2;
  l->next->prev = l;

  return list.next;
}

/*将链表按照参数中设定比较方式排序*/
static GList* 
g_list_sort_real (GList    *list,
		  GFunc     compare_func,
		  gpointer  user_data)
{
  GList *l1, *l2;
  
  if (!list) 
    return NULL;
  if (!list->next) 
    return list;

  /*将链表L拆成前后两半，分别由L1与L2所指*/
  l1 = list; 
  l2 = list->next;

  while ((l2 = l2->next) != NULL)
  {
      if ((l2 = l2->next) == NULL) 
		break;
      l1 = l1->next;
  }
  l2 = l1->next; 
  l1->next = NULL; 

  /*递归的方式处理两段子链表，实现整个链表按照参数中的比较方式排序*/
  return g_list_sort_merge (g_list_sort_real (list, compare_func, user_data),
			    g_list_sort_real (l2, compare_func, user_data),
			    compare_func,
			    user_data);
}

/*将链表按照参数中设定比较方式排序*/
GList *
g_list_sort (GList        *list,
	     GCompareFunc  compare_func)
{
  return g_list_sort_real (list, (GFunc) compare_func, NULL);
			    
}

/*获取链表中元素的个数 */
guint
g_list_length (GList *list)
{
  guint length;
  
  length = 0;
  while (list)
    {
      length++;
      list = list->next;
    }
  
  return length;
}

/*遍历链表，对链表中的每一个元素调用参数中的函数*/
void
g_list_foreach (GList	 *list,
		GFunc	  func,
		gpointer  user_data)
{
  while (list)
    {
      GList *next = list->next;
      (*func) (list->data, user_data);
      list = next;
    }
}

/*释放链表中每一个元素的数据结构，并释放链表空间*/
void
g_list_free_full (GList          *list,
		  GDestroyNotify  free_func)
{
  g_list_foreach (list, (GFunc) free_func, NULL);
  g_list_free (list);
}

/*根据参数中的功能函数找到链表中满足要求的元素*/
GList*
g_list_find_custom (GList         *list,
		    gconstpointer  data,
		    GCompareFunc   func)
{
  g_return_val_if_fail (func != NULL, list);

  while (list)
    {
      if (! func (list->data, data))
		return list;
      list = list->next;
    }

  return NULL;
}

/*删除链表中特定的元素，并且释放其空间 */
GList*
g_list_remove (GList         *list,
               gconstpointer  data)
{
  GList *tmp;
 
  tmp = list;
  while (tmp)
    {
      if (tmp->data != data)
        tmp = tmp->next;
      else
        {
          list = _g_list_remove_link(list, tmp);
          g_list_free(tmp);

          break;
        }
    }
  return list;
}

