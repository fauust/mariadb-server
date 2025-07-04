/* Copyright (c) 2002, 2016, Oracle and/or its affiliates.
   Copyright (c) 2010, 2022, MariaDB

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA */

/**
  @file

  @brief
  subselect Item

  @todo
    - add function from mysql_select that use JOIN* as parameter to JOIN
    methods (sql_select.h/sql_select.cc)
*/

#ifdef USE_PRAGMA_IMPLEMENTATION
#pragma implementation				// gcc: Class implementation
#endif

#include "mariadb.h"
#include "sql_priv.h"
/*
  It is necessary to include set_var.h instead of item.h because there
  are dependencies on include order for set_var.h and item.h. This
  will be resolved later.
*/
#include "sql_class.h"                          // set_var.h: THD
#include "set_var.h"
#include "sql_select.h"
#include "sql_parse.h"                          // check_stack_overrun
#include "sql_cte.h"
#include "sql_test.h"
#include "my_json_writer.h"

double get_post_group_estimate(JOIN* join, double join_op_rows);

LEX_CSTRING exists_outer_expr_name= { STRING_WITH_LEN("<exists outer expr>") };

LEX_CSTRING no_matter_name= {STRING_WITH_LEN("<no matter>") };

int check_and_do_in_subquery_rewrites(JOIN *join);

Item_subselect::Item_subselect(THD *thd_arg):
  Item_result_field(thd_arg), Used_tables_and_const_cache(),
  value_assigned(0), own_engine(0), thd(0), old_engine(0),
  have_to_be_excluded(0),
  inside_first_fix_fields(0), done_first_fix_fields(FALSE), 
  expr_cache(0), forced_const(FALSE), expensive_fl(FALSE),
  substitution(0), engine(0), eliminated(FALSE),
  changed(0), is_correlated(FALSE), with_recursive_reference(0)
{
  DBUG_ENTER("Item_subselect::Item_subselect");
  DBUG_PRINT("enter", ("this: %p", this));
  sortbuffer.str= 0;

#ifndef DBUG_OFF
  exec_counter= 0;
#endif
  with_flags|= item_with_t::SUBQUERY;
  reset();
  /*
    Item value is NULL if select_result_interceptor didn't change this value
    (i.e. some rows will be found returned)
  */
  null_value= TRUE;
  DBUG_VOID_RETURN;
}


void Item_subselect::init(st_select_lex *select_lex,
			  select_result_interceptor *result)
{
  /*
    Please see Item_singlerow_subselect::invalidate_and_restore_select_lex(),
    which depends on alterations to the parse tree implemented here.
  */

  DBUG_ENTER("Item_subselect::init");
  DBUG_PRINT("enter", ("select_lex: %p  this: %p",
                       select_lex, this));

  select_lex->parent_lex->relink_hack(select_lex);

  unit= select_lex->master_unit();

  if (unit->item)
  {
    engine= unit->item->engine;
    parsing_place= unit->item->parsing_place;
    if (unit->item->substype() == EXISTS_SUBS &&
        ((Item_exists_subselect *)unit->item)->exists_transformed)
    {
      /* it is permanent transformation of EXISTS to IN */
      unit->item= this;
      engine->change_result(this, result, FALSE);
    }
    else
    {
      /*
        Item can be changed in JOIN::prepare while engine in JOIN::optimize
        => we do not copy old_engine here
      */
      unit->thd->change_item_tree((Item**)&unit->item, this);
      engine->change_result(this, result, TRUE);
    }
  }
  else
  {
    SELECT_LEX *outer_select= unit->outer_select();
    THD *thd= unit->thd;
    /*
      do not take into account expression inside aggregate functions because
      they can access original table fields
    */
    parsing_place= (outer_select->in_sum_expr ?
                    NO_MATTER :
                    outer_select->parsing_place);
    if (unit->is_unit_op() &&
        (unit->first_select()->next_select() || unit->fake_select_lex))
      engine= new (thd->mem_root)
        subselect_union_engine(unit, result, this);
    else
      engine= new (thd->mem_root)
        subselect_single_select_engine(select_lex, result, this);
  }
  DBUG_PRINT("info", ("engine: %p", engine));
  DBUG_VOID_RETURN;
}

st_select_lex *
Item_subselect::get_select_lex()
{
  return unit->first_select();
}

void Item_subselect::cleanup()
{
  DBUG_ENTER("Item_subselect::cleanup");
  Item_result_field::cleanup();
  if (old_engine)
  {
    if (engine)
      engine->cleanup();
    engine= old_engine;
    old_engine= 0;
  }
  if (engine)
    engine->cleanup();
  reset();
  filesort_buffer.free_sort_buffer();
  my_free(sortbuffer.str);
  sortbuffer.str= 0;

  value_assigned= 0;
  expr_cache= 0;
  forced_const= FALSE;
  DBUG_PRINT("info", ("exec_counter: %d", exec_counter));
#ifndef DBUG_OFF
  exec_counter= 0;
#endif
  DBUG_VOID_RETURN;
}


void Item_singlerow_subselect::cleanup()
{
  DBUG_ENTER("Item_singlerow_subselect::cleanup");
  value= 0; row= 0;
  Item_subselect::cleanup();
  DBUG_VOID_RETURN;
}


void Item_in_subselect::cleanup()
{
  DBUG_ENTER("Item_in_subselect::cleanup");
  if (left_expr_cache)
  {
    left_expr_cache->delete_elements();
    delete left_expr_cache;
    left_expr_cache= NULL;
  }
  /*
    TODO: This breaks the commented assert in add_strategy().
    in_strategy&= ~SUBS_STRATEGY_CHOSEN;
  */
  first_execution= TRUE;
  materialization_tracker= NULL;
  pushed_cond_guards= NULL;
  Item_subselect::cleanup();
  DBUG_VOID_RETURN;
}


void Item_allany_subselect::cleanup()
{
  /*
    The MAX/MIN transformation through injection is reverted through the
    change_item_tree() mechanism. Revert the select_lex object of the
    query to its initial state.
  */
  for (SELECT_LEX *sl= unit->first_select();
       sl; sl= sl->next_select())
    if (test_set_strategy(SUBS_MAXMIN_INJECTED))
      sl->with_sum_func= false;
  Item_in_subselect::cleanup();
}


Item_subselect::~Item_subselect()
{
  DBUG_ENTER("Item_subselect::~Item_subselect");
  DBUG_PRINT("enter", ("this: %p", this));
  if (own_engine)
    delete engine;
  else
    if (engine)  // can be empty in case of EOM
      engine->cleanup();
  engine= NULL;
  DBUG_VOID_RETURN;
}

bool
Item_subselect::select_transformer(JOIN *join)
{
  DBUG_ENTER("Item_subselect::select_transformer");
  DBUG_ASSERT(thd == join->thd);
  DBUG_RETURN(false);
}


bool Item_subselect::fix_fields(THD *thd_param, Item **ref)
{
  THD_WHERE save_where= thd_param->where;
  uint8 uncacheable;
  bool res;

  thd= thd_param;

  DBUG_ASSERT(unit->thd == thd);

  {
    SELECT_LEX *upper= unit->outer_select();
    if (upper->parsing_place == IN_HAVING)
      upper->subquery_in_having= 1;
    /* The subquery is an expression cache candidate */
    upper->expr_cache_may_be_used[upper->parsing_place]= TRUE;
  }

  status_var_increment(thd_param->status_var.feature_subquery);

  DBUG_ASSERT(fixed() == 0);
  engine->set_thd((thd= thd_param));
  if (!done_first_fix_fields)
  {
    done_first_fix_fields= TRUE;
    inside_first_fix_fields= TRUE;
    upper_refs.empty();
    /*
      psergey-todo: remove _first_fix_fields calls, we need changes on every
      execution
    */
  }

  eliminated= FALSE;
  parent_select= thd_param->lex->current_select;

  if (check_stack_overrun(thd, STACK_MIN_SIZE, (uchar*)&res))
    return TRUE;
  
  for (SELECT_LEX *sl= unit->first_select(); sl; sl= sl->next_select())
  {
    if (sl->tvc)
    {
      if (!(sl= wrap_tvc_into_select(thd, sl)))
      {
        res= TRUE;
        goto end;
      }
      if (sl ==  unit->first_select() && !sl->next_select())
        unit->fake_select_lex= 0;
    }
  }
  
  if (!(res= engine->prepare(thd)))
  {
    // all transformation is done (used by prepared statements)
    changed= 1;
    inside_first_fix_fields= FALSE;

    /*
      Substitute the current item with an Item_in_optimizer that was
      created by Item_in_subselect::select_in_like_transformer and
      call fix_fields for the substituted item which in turn calls
      engine->prepare for the subquery predicate.
    */
    if (substitution)
    {
      /*
        If the top item of the WHERE/HAVING condition changed,
        set correct WHERE/HAVING for PS.
      */
      if (unit->outer_select()->where == (*ref))
        unit->outer_select()->where= substitution;
      else if (unit->outer_select()->having == (*ref))
        unit->outer_select()->having= substitution;

      (*ref)= substitution;
      substitution->name= name;
      if (have_to_be_excluded)
	engine->exclude();
      substitution= 0;
      thd->where= THD_WHERE::CHECKING_TRANSFORMED_SUBQUERY;
      res= (*ref)->fix_fields_if_needed(thd, ref);
      goto end;

    }
    // Is it one field subselect?
    if (engine->cols() > max_columns)
    {
      my_error(ER_OPERAND_COLUMNS, MYF(0), 1);
      res= TRUE;
      goto end;
    }
    if (fix_length_and_dec())
    {
      res= TRUE;
      goto end;
    }
  }
  else
    goto end;
  
  if ((uncacheable= engine->uncacheable() & ~UNCACHEABLE_EXPLAIN) ||
      with_recursive_reference)
  {
    const_item_cache= 0;
    if (uncacheable & UNCACHEABLE_RAND)
      used_tables_cache|= RAND_TABLE_BIT;
  }
  base_flags|= item_base_t::FIXED;

end:
  done_first_fix_fields= FALSE;
  inside_first_fix_fields= FALSE;
  thd->where= save_where;
  return res;
}


bool Item_subselect::enumerate_field_refs_processor(void *arg)
{
  List_iterator<Ref_to_outside> it(upper_refs);
  Ref_to_outside *upper;
  
  while ((upper= it++))
  {
    if (upper->item &&
        upper->item->walk(&Item::enumerate_field_refs_processor, FALSE, arg))
      return TRUE;
  }
  return FALSE;
}

bool Item_subselect::mark_as_eliminated_processor(void *arg)
{
  eliminated= TRUE;
  return FALSE;
}


/**
  Remove a subselect item from its unit so that the unit no longer
  represents a subquery.

  @param arg  unused parameter

  @return
    FALSE to force the evaluation of the processor for the subsequent items.
*/

bool Item_subselect::eliminate_subselect_processor(void *arg)
{
  unit->item= NULL;
  if (!unit->is_excluded())
    unit->exclude();
  eliminated= TRUE;
  return FALSE;
}


bool Item_subselect::mark_as_dependent(THD *thd, st_select_lex *select, 
                                       Item *item)
{
  if (inside_first_fix_fields)
  {
    is_correlated= TRUE;
    Ref_to_outside *upper;
    if (!(upper= new (thd->mem_root) Ref_to_outside()))
      return TRUE;
    upper->select= select;
    upper->item= item;
    if (upper_refs.push_back(upper, thd->mem_root))
      return TRUE;
  }
  return FALSE;
}


/*
  @brief
    Update the table bitmaps for the outer references used within a subquery
*/

bool Item_subselect::update_table_bitmaps_processor(void *arg)
{
  List_iterator<Ref_to_outside> it(upper_refs);
  Ref_to_outside *upper;

  while ((upper= it++))
  {
    if (upper->item &&
        upper->item->walk(&Item::update_table_bitmaps_processor, FALSE, arg))
      return TRUE;
  }
  return FALSE;
}


/*
  Adjust attributes after our parent select has been merged into grandparent

  DESCRIPTION
    Subquery is a composite object which may be correlated, that is, it may
    have
    1. references to tables of the parent select (i.e. one that has the clause
      with the subquery predicate)
    2. references to tables of the grandparent select
    3. references to tables of further ancestors.
    
    Before the pullout, this item indicates:
    - #1 with table bits in used_tables()
    - #2 and #3 with OUTER_REF_TABLE_BIT.

    After parent has been merged with grandparent:
    - references to parent and grandparent tables should be indicated with 
      table bits.
    - references to greatgrandparent and further ancestors - with
      OUTER_REF_TABLE_BIT.
*/

void Item_subselect::fix_after_pullout(st_select_lex *new_parent,
                                       Item **ref, bool merge)
{
  recalc_used_tables(new_parent, TRUE);
  parent_select= new_parent;
}


class Field_fixer: public Field_enumerator
{
public:
  table_map used_tables; /* Collect used_tables here */
  st_select_lex *new_parent; /* Select we're in */
  void visit_field(Item_field *item) override
  {
    //for (TABLE_LIST *tbl= new_parent->leaf_tables; tbl; tbl= tbl->next_local)
    //{
    //  if (tbl->table == field->table)
    //  {
        used_tables|= item->field->table->map;
    //    return;
    //  }
    //}
    //used_tables |= OUTER_REF_TABLE_BIT;
  }
};


/*
  Recalculate used_tables_cache 
*/

void Item_subselect::recalc_used_tables(st_select_lex *new_parent, 
                                        bool after_pullout)
{
  List_iterator_fast<Ref_to_outside> it(upper_refs);
  Ref_to_outside *upper;
  DBUG_ENTER("recalc_used_tables");
  
  used_tables_cache= 0;
  while ((upper= it++))
  {
    bool found= FALSE;
    /*
      Check if
        1. the upper reference refers to the new immediate parent select, or
        2. one of the further ancestors.

      We rely on the fact that the tree of selects is modified by some kind of
      'flattening', i.e. a process where child selects are merged into their
      parents.
      The merged selects are removed from the select tree but keep pointers to
      their parents.
    */
    for (st_select_lex *sel= upper->select; sel; sel= sel->outer_select())
    {
      /* 
        If we've reached the new parent select by walking upwards from
        reference's original select, this means that the reference is now 
        referring to the direct parent:
      */
      if (sel == new_parent)
      {
        found= TRUE;
        /* 
          upper->item may be NULL when we've referred to a grouping function,
          in which case we don't care about what it's table_map really is,
          because item->with_sum_func==1 will ensure correct placement of the
          item.
        */
        if (upper->item)
        {
          // Now, iterate over fields and collect used_tables() attribute:
          Field_fixer fixer;
          fixer.used_tables= 0;
          fixer.new_parent= new_parent;
          upper->item->walk(&Item::enumerate_field_refs_processor, 0, &fixer);
          used_tables_cache |= fixer.used_tables;
          upper->item->walk(&Item::update_table_bitmaps_processor, FALSE, NULL);
/*
          if (after_pullout)
            upper->item->fix_after_pullout(new_parent, &(upper->item));
          upper->item->update_used_tables();
*/          
        }
      }
    }
    if (!found)
      used_tables_cache|= OUTER_REF_TABLE_BIT;
  }
  /* 
    Don't update const_tables_cache yet as we don't yet know which of the
    parent's tables are constant. Parent will call update_used_tables() after
    he has done const table detection, and that will be our chance to update
    const_tables_cache.
  */
  DBUG_PRINT("exit", ("used_tables_cache: %llx", used_tables_cache));
  DBUG_VOID_RETURN;
}


/**
  Determine if a subquery is expensive to execute during query optimization.

  @details The cost of execution of a subquery is estimated based on an
  estimate of the number of rows the subquery will access during execution.
  This measure is used instead of JOIN::read_time, because it is considered
  to be much more reliable than the cost estimate.

  Note: the logic in this function must agree with
  JOIN::init_join_cache_and_keyread().

  @return true if the subquery is expensive
  @return false otherwise
*/
bool Item_subselect::is_expensive()
{
  double examined_rows= 0;
  bool all_are_simple= true;

  if (!expensive_fl && is_evaluated())
    return false;

  /* check extremely simple select */
  if (!unit->first_select()->next_select()) // no union
  {
    /*
      such single selects works even without optimization because
      can not makes loops
    */
    SELECT_LEX *sl= unit->first_select();
    JOIN *join = sl->join;
    if (join && !join->tables_list && !sl->first_inner_unit())
      return (expensive_fl= false);
  }


  for (SELECT_LEX *sl= unit->first_select(); sl; sl= sl->next_select())
  {
    JOIN *cur_join= sl->join;

    /* not optimized subquery */
    if (!cur_join)
      return (expensive_fl= true);

    /*
      If the subquery is not optimised or in the process of optimization
      it supposed to be expensive
    */
    if (cur_join->optimization_state != JOIN::OPTIMIZATION_DONE)
      return (expensive_fl= true);

    if (!cur_join->tables_list && !sl->first_inner_unit())
      continue;

    /*
      Subqueries whose result is known after optimization are not expensive.
      Such subqueries have all tables optimized away, thus have no join plan.
    */
    if ((cur_join->zero_result_cause || !cur_join->tables_list))
      continue;

    /*
      This is not simple SELECT in union so we can not go by simple condition
    */
    all_are_simple= false;

    /*
      If a subquery is not optimized we cannot estimate its cost. A subquery is
      considered optimized if it has a join plan.
    */
    if (!cur_join->join_tab)
      return (expensive_fl= true);

    if (sl->first_inner_unit())
    {
      /*
        Subqueries that contain subqueries are considered expensive.
        @todo: accumulate the cost of subqueries.
      */
      return (expensive_fl= true);
    }

    examined_rows+= cur_join->get_examined_rows();
  }

  // here we are sure that subquery is optimized so thd is set
  return (expensive_fl= !all_are_simple &&
	   (examined_rows > thd->variables.expensive_subquery_limit));
}


/*
  @brief
    Apply item processor for all scalar (i.e. Item*) expressions that
    occur in the nested join.
*/

static
int walk_items_for_table_list(Item_processor processor,
                              bool walk_subquery, void *argument,
                              List<TABLE_LIST>& join_list)
{
  List_iterator<TABLE_LIST> li(join_list);
  int res;
  while (TABLE_LIST *table= li++)
  {
    if (table->on_expr)
    {
      if ((res= table->on_expr->walk(processor, walk_subquery, argument)))
        return res;
    }
    if (Table_function_json_table *tf= table->table_function)
    {
      if ((res= tf->walk_items(processor, walk_subquery, argument)))
      {
        return res;
      }
    }

    if (table->nested_join)
    {
      if ((res= walk_items_for_table_list(processor, walk_subquery, argument,
                                          table->nested_join->join_list)))
        return res;
    }
  }
  return 0;
}


bool Item_subselect::unknown_splocal_processor(void *argument)
{
  SELECT_LEX *sl= unit->first_select();
  if (sl->top_join_list.elements)
    return 0;
  if (sl->tvc && sl->tvc->walk_values(&Item::unknown_splocal_processor,
                                      false, argument))
    return true;
  for (SELECT_LEX *lex= unit->first_select(); lex; lex= lex->next_select())
  {
    /*
      TODO: walk through GROUP BY and ORDER yet eventually.
      This will require checking aliases in SELECT list:
        SELECT 1 AS a GROUP BY a;
        SELECT 1 AS a ORDER BY a;
    */
    List_iterator<Item> li(lex->item_list);
    Item *item;
    if (lex->where && (lex->where)->walk(&Item::unknown_splocal_processor,
                                         false, argument))
      return true;
    if (lex->having && (lex->having)->walk(&Item::unknown_splocal_processor,
                                           false, argument))
      return true;
    while ((item=li++))
    {
      if (item->walk(&Item::unknown_splocal_processor, false, argument))
        return true;
    }
  }
  return false;
}


bool Item_subselect::walk(Item_processor processor, bool walk_subquery,
                          void *argument)
{
  if (!(unit->uncacheable & ~UNCACHEABLE_DEPENDENT) && engine->is_executed() &&
      !unit->describe)
  {
    /*
      The subquery has already been executed (for real, it wasn't EXPLAIN's
      fake execution) so it should not matter what it has inside.
      
      The actual reason for not walking inside is that parts of the subquery
      (e.g. JTBM join nests and their IN-equality conditions may have been 
       invalidated by irreversible cleanups (those happen after an uncorrelated 
       subquery has been executed).
    */
    return (this->*processor)(argument);
  }

  if (walk_subquery)
  {
    for (SELECT_LEX *lex= unit->first_select(); lex; lex= lex->next_select())
    {
      List_iterator<Item> li(lex->item_list);
      ORDER *order;

      if (lex->where && (lex->where)->walk(processor, walk_subquery, argument))
        return 1;
      if (lex->having && (lex->having)->walk(processor, walk_subquery,
                                             argument))
        return 1;

      if (walk_items_for_table_list(processor, walk_subquery, argument,
                                    *lex->join_list))
        return 1;

      while (Item *item= li++)
      {
        if (item->walk(processor, walk_subquery, argument))
          return 1;
      }
      for (order= lex->order_list.first ; order; order= order->next)
      {
        if ((*order->item)->walk(processor, walk_subquery, argument))
          return 1;
      }
      for (order= lex->group_list.first ; order; order= order->next)
      {
        if ((*order->item)->walk(processor, walk_subquery, argument))
          return 1;
      }
    }
  }
  return (this->*processor)(argument);
}


bool Item_subselect::exec()
{
  subselect_engine *org_engine= engine;
  DBUG_ENTER("Item_subselect::exec");
  DBUG_ASSERT(fixed());
  DBUG_ASSERT(thd);
  DBUG_ASSERT(!eliminated);

  DBUG_EXECUTE_IF("Item_subselect",
    Item::Print print(this,
      enum_query_type(QT_TO_SYSTEM_CHARSET |
        QT_WITHOUT_INTRODUCERS));

    push_warning_printf(thd, Sql_condition::WARN_LEVEL_NOTE,
                        ER_UNKNOWN_ERROR, "DBUG: Item_subselect::exec %.*b",
                        print.length(),print.ptr());
  );
  /*
    Do not execute subselect in case of a fatal error
    or if the query has been killed.
  */
  if (unlikely(thd->is_error() || thd->killed))
    DBUG_RETURN(true);

  DBUG_ASSERT(!thd->lex->context_analysis_only);
  /*
    Simulate a failure in sub-query execution. Used to test e.g.
    out of memory or query being killed conditions.
  */
  DBUG_EXECUTE_IF("subselect_exec_fail", DBUG_RETURN(true););

  bool res= engine->exec();

#ifndef DBUG_OFF
  ++exec_counter;
#endif
  if (engine != org_engine)
  {
    /*
      If the subquery engine changed during execution due to lazy subquery
      optimization, or because the original engine found a more efficient other
      engine, re-execute the subquery with the new engine.
    */
    DBUG_RETURN(exec());
  }
  DBUG_RETURN(res);
}


void Item_subselect::get_cache_parameters(List<Item> &parameters)
{
  Collect_deps_prm prm= {&parameters,      // parameters
    unit->first_select()->nest_level_base, // nest_level_base
    0,                                     // count
    unit->first_select()->nest_level,      // nest_level
    TRUE                                   // collect
  };
  walk(&Item::collect_outer_ref_processor, TRUE, &prm);
}

int Item_in_subselect::optimize(double *out_rows, double *cost)
{
  int res;
  DBUG_ENTER("Item_in_subselect::optimize");
  DBUG_ASSERT(fixed());
  SELECT_LEX *save_select= thd->lex->current_select;
  JOIN *join= unit->first_select()->join;

  thd->lex->current_select= join->select_lex;
  if ((res= join->optimize()))
    DBUG_RETURN(res);

  /* Calculate #rows and cost of join execution */
  join->get_partial_cost_and_fanout(join->table_count - join->const_tables, 
                                    table_map(-1),
                                    cost, out_rows);

  /*
    Adjust join output cardinality. There can be these cases:
    - Have no GROUP BY and no aggregate funcs: we won't get into this 
      function because such join will be processed as a merged semi-join 
      (TODO: does it really mean we don't need to handle such cases here at 
       all? put ASSERT)
    - Have no GROUP BY but have aggregate funcs: output is 1 record.
    - Have GROUP BY and have (or not) aggregate funcs:  need to adjust output 
      cardinality.
  */
  thd->lex->current_select= save_select;
  if (!join->group_list && !join->group_optimized_away &&
      join->tmp_table_param.sum_func_count)
  {
    DBUG_PRINT("info",("Materialized join will have only 1 row (it has "
                       "aggregates but no GROUP BY"));
    *out_rows= 1;
  }
  
  /* Now with grouping */
  if (join->group_list_for_estimates)
  {
    DBUG_PRINT("info",("Materialized join has grouping, trying to estimate it"));
    double output_rows= get_post_group_estimate(join, *out_rows);
    DBUG_PRINT("info",("Got value of %g", output_rows));
    *out_rows= output_rows;
  }

  DBUG_RETURN(res);

}


/**
  Check if an expression cache is needed for this subquery

  @param thd             Thread handle

  @details
  The function checks whether a cache is needed for a subquery and whether
  the result of the subquery can be put in cache.

  @retval TRUE  cache is needed
  @retval FALSE otherwise
*/

bool Item_subselect::expr_cache_is_needed(THD *thd)
{
  return ((engine->uncacheable() & UNCACHEABLE_DEPENDENT) &&
          engine->cols() == 1 &&
          optimizer_flag(thd, OPTIMIZER_SWITCH_SUBQUERY_CACHE) &&
          !(engine->uncacheable() & (UNCACHEABLE_RAND |
                                     UNCACHEABLE_SIDEEFFECT)) &&
          !with_recursive_reference);
}


/**
  Check if the left IN argument contains NULL values.

  @retval TRUE  there are NULLs
  @retval FALSE otherwise
*/

inline bool Item_in_subselect::left_expr_has_null()
{
  return (*(optimizer->get_cache()))->null_value_inside;
}


/**
  Check if an expression cache is needed for this subquery

  @param thd             Thread handle

  @details
  The function checks whether a cache is needed for a subquery and whether
  the result of the subquery can be put in cache.

  @note
  This method allows many columns in the subquery because it is supported by
  Item_in_optimizer and result of the IN subquery will be scalar in this
  case.

  @retval TRUE  cache is needed
  @retval FALSE otherwise
*/

bool Item_in_subselect::expr_cache_is_needed(THD *thd)
{
  return (optimizer_flag(thd, OPTIMIZER_SWITCH_SUBQUERY_CACHE) &&
          !(engine->uncacheable() & (UNCACHEABLE_RAND |
                                     UNCACHEABLE_SIDEEFFECT)) &&
          !with_recursive_reference);
}


/*
  Compute the IN predicate if the left operand's cache changed.
*/

bool Item_in_subselect::exec()
{
  DBUG_ENTER("Item_in_subselect::exec");
  DBUG_ASSERT(fixed());
  DBUG_ASSERT(thd);

  /*
    Initialize the cache of the left predicate operand. This has to be done as
    late as now, because Cached_item directly contains a resolved field (not
    an item, and in some cases (when temp tables are created), these fields
    end up pointing to the wrong field. One solution is to change Cached_item
    to not resolve its field upon creation, but to resolve it dynamically
    from a given Item_ref object.
    TODO: the cache should be applied conditionally based on:
    - rules - e.g. only if the left operand is known to be ordered, and/or
    - on a cost-based basis, that takes into account the cost of a cache
      lookup, the cache hit rate, and the savings per cache hit.
  */
  if (!left_expr_cache && (test_strategy(SUBS_MATERIALIZATION)))
    init_left_expr_cache();

  /*
    If the new left operand is already in the cache, reuse the old result.
    Use the cached result only if this is not the first execution of IN
    because the cache is not valid for the first execution.
  */
  if (!first_execution && left_expr_cache &&
      test_if_item_cache_changed(*left_expr_cache) < 0)
    DBUG_RETURN(FALSE);

  /*
    The exec() method below updates item::value, and item::null_value, thus if
    we don't call it, the next call to item::val_int() will return whatever
    result was computed by its previous call.
  */
  DBUG_RETURN(Item_subselect::exec());
}


Item::Type Item_subselect::type() const
{
  return SUBSELECT_ITEM;
}


bool Item_subselect::fix_length_and_dec()
{
  if (engine->fix_length_and_dec(0))
    return TRUE;
  return FALSE;
}


table_map Item_subselect::used_tables() const
{
  return (table_map) ((engine->uncacheable() & ~UNCACHEABLE_EXPLAIN)? 
                      used_tables_cache : 0L);
}


bool Item_subselect::const_item() const
{
  DBUG_ASSERT(thd);
  return (thd->lex->context_analysis_only || with_recursive_reference ?
          FALSE :
          forced_const || const_item_cache);
}

Item *Item_subselect::get_tmp_table_item(THD *thd_arg)
{
  if (!with_sum_func() && !const_item())
  {
    auto item_field=
        new (thd->mem_root) Item_field(thd_arg, result_field);
    if (item_field)
      item_field->set_refers_to_temp_table();
    return item_field;
  }
  return copy_or_same(thd_arg);
}

void Item_subselect::update_used_tables()
{
  if (!forced_const)
  {
    recalc_used_tables(parent_select, FALSE);
    if (!(engine->uncacheable() & ~UNCACHEABLE_EXPLAIN))
    {
      // did all used tables become static?
      if (!(used_tables_cache & ~engine->upper_select_const_tables()) &&
          ! with_recursive_reference)
        const_item_cache= 1;
    }
  }
}


void Item_subselect::print(String *str, enum_query_type query_type)
{
  if (query_type & QT_ITEM_SUBSELECT_ID_ONLY)
  {
    str->append(STRING_WITH_LEN("(subquery#"));
    if (unit && unit->first_select())
    {
      char buf[64];
      size_t length= (size_t)
        (longlong10_to_str(unit->first_select()->select_number, buf, 10) -
         buf);
      str->append(buf, length);
    }
    else
    {
      // TODO: Explain what exactly does this mean?
      str->append(NULL_clex_str);
    }

    str->append(')');
    return;
  }
  if (engine)
  {
    str->append('(');
    engine->print(str, query_type);
    str->append(')');
  }
  else
    str->append(STRING_WITH_LEN("(...)"));
}


Item_singlerow_subselect::Item_singlerow_subselect(THD *thd, st_select_lex *select_lex):
  Item_subselect(thd), value(0)
{
  DBUG_ENTER("Item_singlerow_subselect::Item_singlerow_subselect");
  init(select_lex, new (thd->mem_root) select_singlerow_subselect(thd, this));
  set_maybe_null();
  max_columns= UINT_MAX;
  DBUG_VOID_RETURN;
}

st_select_lex *
Item_singlerow_subselect::invalidate_and_restore_select_lex()
{
  DBUG_ENTER("Item_singlerow_subselect::invalidate_and_restore_select_lex");
  st_select_lex *result= get_select_lex();

  DBUG_ASSERT(result);

  /*
    This code restore the parse tree in it's state before the execution of
    Item_singlerow_subselect::Item_singlerow_subselect(),
    and in particular decouples this object from the SELECT_LEX,
    so that the SELECT_LEX can be used with a different flavor
    or Item_subselect instead, as part of query rewriting.
  */
  unit->item= NULL;

  DBUG_RETURN(result);
}

Item_maxmin_subselect::Item_maxmin_subselect(THD *thd,
                                             Item_subselect *parent,
					     st_select_lex *select_lex,
					     bool max_arg):
  Item_singlerow_subselect(thd), was_values(TRUE)
{
  DBUG_ENTER("Item_maxmin_subselect::Item_maxmin_subselect");
  max= max_arg;
  init(select_lex,
       new (thd->mem_root) select_max_min_finder_subselect(thd,
             this, max_arg, parent->substype() == Item_subselect::ALL_SUBS));
  max_columns= 1;
  set_maybe_null();
  max_columns= 1;

  /*
    Following information was collected during performing fix_fields()
    of Items belonged to subquery, which will be not repeated
  */
  used_tables_cache= parent->get_used_tables_cache();
  const_item_cache= parent->const_item();

  DBUG_VOID_RETURN;
}

void Item_maxmin_subselect::cleanup()
{
  DBUG_ENTER("Item_maxmin_subselect::cleanup");
  Item_singlerow_subselect::cleanup();

  /*
    By default it is TRUE to avoid TRUE reporting by
    Item_func_not_all/Item_func_nop_all if this item was never called.

    Engine exec() set it to FALSE by reset_value_registration() call.
    select_max_min_finder_subselect::send_data() set it back to TRUE if some
    value will be found.
  */
  was_values= TRUE;
  DBUG_VOID_RETURN;
}


void Item_maxmin_subselect::print(String *str, enum_query_type query_type)
{
  str->append(max?"<max>":"<min>", 5);
  Item_singlerow_subselect::print(str, query_type);
}


void Item_maxmin_subselect::no_rows_in_result()
{
  /*
    Subquery predicates outside of the SELECT list must be evaluated in order
    to possibly filter the special result row generated for implicit grouping
    if the subquery is in the HAVING clause.
    If the predicate is constant, we need its actual value in the only result
    row for queries with implicit grouping.
  */
  if (parsing_place != SELECT_LIST || const_item())
    return;
  value= get_cache(thd);
  null_value= 0;
  was_values= 0;
  make_const();
}


void Item_singlerow_subselect::no_rows_in_result()
{
  /*
    Subquery predicates outside of the SELECT list must be evaluated in order
    to possibly filter the special result row generated for implicit grouping
    if the subquery is in the HAVING clause.
    If the predicate is constant, we need its actual value in the only result
    row for queries with implicit grouping.
  */
  if (parsing_place != SELECT_LIST || const_item())
    return;
  value= get_cache(thd);
  reset();
  make_const();
}


void Item_singlerow_subselect::reset()
{
  Item_subselect::reset();
  if (value)
  {
    for(uint i= 0; i < engine->cols(); i++)
      row[i]->set_null();
  }
}


/**
  @todo
  - We can't change name of Item_field or Item_ref, because it will
  prevent its correct resolving, but we should save name of
  removed item => we do not make optimization if top item of
  list is field or reference.
  - switch off this optimization for prepare statement,
  because we do not rollback these changes.
  Make rollback for it, or special name resolving mode in 5.0.

  @param join  Join object of the subquery (i.e. 'child' join).

  @retval false  The subquery was transformed
*/
bool
Item_singlerow_subselect::select_transformer(JOIN *join)
{
  DBUG_ENTER("Item_singlerow_subselect::select_transformer");
  if (changed)
    DBUG_RETURN(false);
  DBUG_ASSERT(join->thd == thd);

  SELECT_LEX *select_lex= join->select_lex;
  Query_arena *arena, backup;
  arena= thd->activate_stmt_arena_if_needed(&backup);

  auto need_to_pull_out_item = [](enum_parsing_place context_analysis_place,
                                  Item *item) {
    return
      !item->with_sum_func() &&
      !item->with_window_func() &&
      /*
        We can't change name of Item_field or Item_ref, because it will
        prevent its correct resolving, but we should save name of
        removed item => we do not make optimization if top item of
        list is field or reference.
        TODO: solve above problem
      */
      item->type() != FIELD_ITEM && item->type() != REF_ITEM &&
      /*
        The item can be pulled out to upper level in case it doesn't represent
        the constant in the clause 'ORDER/GROUP BY (constant)'.
      */
      !((item->is_order_clause_position() ||
         item->is_stored_routine_parameter()) &&
        (context_analysis_place == IN_ORDER_BY ||
         context_analysis_place == IN_GROUP_BY)
       );
  };

  if (!select_lex->master_unit()->is_unit_op() &&
      !select_lex->table_list.elements &&
      select_lex->item_list.elements == 1 &&
      !join->conds && !join->having &&
      need_to_pull_out_item(
        join->select_lex->outer_select()->context_analysis_place,
        select_lex->item_list.head()) &&
      thd->stmt_arena->state != Query_arena::STMT_INITIALIZED_FOR_SP)
  {
    have_to_be_excluded= 1;
    if (thd->lex->describe)
    {
      char warn_buff[MYSQL_ERRMSG_SIZE];
      sprintf(warn_buff, ER_THD(thd, ER_SELECT_REDUCED),
              select_lex->select_number);
      push_warning(thd, Sql_condition::WARN_LEVEL_NOTE,
		   ER_SELECT_REDUCED, warn_buff);
    }
    substitution= select_lex->item_list.head();
    /*
      as far as we moved content to upper level we have to fix dependences & Co
    */
    substitution->fix_after_pullout(select_lex->outer_select(),
                                    &substitution, TRUE);
  }
  if (arena)
    thd->restore_active_arena(arena, &backup);
  DBUG_RETURN(false);
}


void Item_singlerow_subselect::store(uint i, Item *item)
{
  row[i]->store(item);
  row[i]->cache_value();
}

const Type_handler *Item_singlerow_subselect::type_handler() const
{
  return engine->type_handler();
}

bool Item_singlerow_subselect::fix_length_and_dec()
{
  if ((max_columns= engine->cols()) == 1)
  {
    if (engine->fix_length_and_dec(row= &value))
      return TRUE;
  }
  else
  {
    if (!(row= (Item_cache**) current_thd->alloc(sizeof(Item_cache*) *
                                                 max_columns)) ||
        engine->fix_length_and_dec(row))
      return TRUE;
    value= *row;
  }
  unsigned_flag= value->unsigned_flag;
  /*
    If the subquery always returns a row, like "(SELECT subq_value)"
    then its NULLability is the same as subq_value's NULLability.
  */
  if (engine->always_returns_one_row())
    set_maybe_null(engine->may_be_null());
  else
  {
    for (uint i= 0; i < max_columns; i++)
      row[i]->set_maybe_null();
  }
  return FALSE;
}



/*
  @brief
     Check if we can guarantee that this engine will always produce exactly one
     row.

  @detail
    Check if the subquery is just

      (SELECT value)

    Then we can guarantee we always return one row.
    Selecting from tables may produce more than one row.
    HAVING, WHERE or ORDER BY/LIMIT clauses may cause no rows to be produced.
*/

bool subselect_single_select_engine::always_returns_one_row() const
{
  st_select_lex *params= select_lex->master_unit()->global_parameters();
  return no_tables() &&
         !params->limit_params.select_limit &&
         !params->limit_params.offset_limit &&
         !select_lex->where &&
         !select_lex->having;
}

/**
  Add an expression cache for this subquery if it is needed

  @param thd_arg         Thread handle

  @details
  The function checks whether an expression cache is needed for this item
  and if if so wraps the item into an item of the class
  Item_cache_wrapper with an appropriate expression cache set up there.

  @note
  used from Item::transform()

  @return
  new wrapper item if an expression cache is needed,
  this item - otherwise
*/

Item* Item_singlerow_subselect::expr_cache_insert_transformer(THD *tmp_thd,
                                                              uchar *unused)
{
  DBUG_ENTER("Item_singlerow_subselect::expr_cache_insert_transformer");

  DBUG_ASSERT(thd == tmp_thd);

  /*
    Do not create subquery cache if the subquery was eliminated.
    The optimizer may eliminate subquery items (see
    eliminate_subselect_processor). However it does not update
    all query's data structures, so the eliminated item may be
    still reachable.
  */
  if (eliminated)
    DBUG_RETURN(this);

  if (expr_cache)
    DBUG_RETURN(expr_cache);

  if (expr_cache_is_needed(tmp_thd) &&
      (expr_cache= set_expr_cache(tmp_thd)))
  {
    init_expr_cache_tracker(tmp_thd);
    DBUG_RETURN(expr_cache);
  }
  DBUG_RETURN(this);
}


uint Item_singlerow_subselect::cols() const
{
  return engine->cols();
}

bool Item_singlerow_subselect::check_cols(uint c)
{
  if (c != engine->cols())
  {
    my_error(ER_OPERAND_COLUMNS, MYF(0), c);
    return 1;
  }
  return 0;
}

bool Item_singlerow_subselect::null_inside()
{
  for (uint i= 0; i < max_columns ; i++)
  {
    if (row[i]->null_value)
      return 1;
  }
  return 0;
}

void Item_singlerow_subselect::bring_value()
{
  if (!exec() && assigned())
  {
    null_value= true;
    for (uint i= 0; i < max_columns ; i++)
    {
      if (!row[i]->null_value)
      {
        null_value= false;
        return;
      }
    }
  }
  else
    reset();
}

double Item_singlerow_subselect::val_real()
{
  DBUG_ASSERT(fixed());
  if (forced_const)
    return value->val_real();
  if (!exec() && !value->null_value)
  {
    null_value= FALSE;
    return value->val_real();
  }
  else
  {
    reset();
    return 0;
  }
}

longlong Item_singlerow_subselect::val_int()
{
  DBUG_ASSERT(fixed());
  if (forced_const)
  {
    longlong val= value->val_int();
    null_value= value->null_value;
    return val;
  }
  if (!exec() && !value->null_value)
  {
    null_value= FALSE;
    return value->val_int();
  }
  else
  {
    reset();
    DBUG_ASSERT(null_value);
    return 0;
  }
}

String *Item_singlerow_subselect::val_str(String *str)
{
  DBUG_ASSERT(fixed());
  if (forced_const)
  {
    String *res= value->val_str(str);
    null_value= value->null_value;
    return res;
  }
  if (!exec() && !value->null_value)
  {
    null_value= FALSE;
    return value->val_str(str);
  }
  else
  {
    reset();
    DBUG_ASSERT(null_value);
    return 0;
  }
}


bool Item_singlerow_subselect::val_native(THD *thd, Native *to)
{
  DBUG_ASSERT(fixed());
  if (forced_const)
    return value->val_native(thd, to);
  if (!exec() && !value->null_value)
  {
    null_value= false;
    return value->val_native(thd, to);
  }
  else
  {
    reset();
    return true;
  }
}


my_decimal *Item_singlerow_subselect::val_decimal(my_decimal *decimal_value)
{
  DBUG_ASSERT(fixed());
  if (forced_const)
  {
    my_decimal *val= value->val_decimal(decimal_value);
    null_value= value->null_value;
    return val;
  }
  if (!exec() && !value->null_value)
  {
    null_value= FALSE;
    return value->val_decimal(decimal_value);
  }
  else
  {
    reset();
    DBUG_ASSERT(null_value);
    return 0;
  }
}


bool Item_singlerow_subselect::val_bool()
{
  DBUG_ASSERT(fixed());
  if (forced_const)
  {
    bool val= value->val_bool();
    null_value= value->null_value;
    return val;
  }
  if (!exec() && !value->null_value)
  {
    null_value= FALSE;
    return value->val_bool();
  }
  else
  {
    reset();
    DBUG_ASSERT(null_value);
    return 0;
  }
}


bool Item_singlerow_subselect::get_date(THD *thd, MYSQL_TIME *ltime, date_mode_t fuzzydate)
{
  DBUG_ASSERT(fixed());
  if (forced_const)
  {
    bool val= value->get_date(thd, ltime, fuzzydate);
    null_value= value->null_value;
    return val;
  }
  if (!exec() && !value->null_value)
  {
    null_value= FALSE;
    return value->get_date(thd, ltime, fuzzydate);
  }
  else
  {
    reset();
    DBUG_ASSERT(null_value);
    return 1;
  }
}


Item_exists_subselect::Item_exists_subselect(THD *thd,
                                             st_select_lex *select_lex):
  Item_subselect(thd), upper_not(NULL),
  emb_on_expr_nest(NULL), optimizer(0), exists_transformed(0)
{
  DBUG_ENTER("Item_exists_subselect::Item_exists_subselect");


  init(select_lex, new (thd->mem_root) select_exists_subselect(thd, this));
  max_columns= UINT_MAX;
  null_value= FALSE; //can't be NULL
  base_flags&= ~item_base_t::MAYBE_NULL; //can't be NULL
  value= 0;
  DBUG_VOID_RETURN;
}


void Item_exists_subselect::print(String *str, enum_query_type query_type)
{
  str->append(STRING_WITH_LEN("exists"));
  Item_subselect::print(str, query_type);
}


bool Item_in_subselect::test_limit(st_select_lex_unit *unit_arg)
{
  if (unlikely(unit_arg->fake_select_lex &&
               unit_arg->fake_select_lex->test_limit()))
    return(1);

  SELECT_LEX *sl= unit_arg->first_select();
  for (; sl; sl= sl->next_select())
  {
    if (unlikely(sl->test_limit()))
      return(1);
  }
  return(0);
}

Item_in_subselect::Item_in_subselect(THD *thd, Item * left_exp,
				     st_select_lex *select_lex):
  Item_exists_subselect(thd), left_expr_cache(0), first_execution(TRUE),
  in_strategy(SUBS_NOT_TRANSFORMED),
  materialization_tracker(NULL),
  pushed_cond_guards(NULL), do_not_convert_to_sj(FALSE), is_jtbm_merged(FALSE),
  is_jtbm_const_tab(FALSE), is_flattenable_semijoin(FALSE),
  is_registered_semijoin(FALSE),
  upper_item(0),
  converted_from_in_predicate(FALSE)
{
  DBUG_ENTER("Item_in_subselect::Item_in_subselect");
  DBUG_PRINT("info", ("in_strategy: %u", (uint)in_strategy));

  left_expr_orig= left_expr= left_exp;
  /* prepare to possible disassembling the item in convert_subq_to_sj() */
  if (left_exp->type() == Item::ROW_ITEM)
    left_expr_orig= new (thd->mem_root)
      Item_row(thd, static_cast<Item_row*>(left_exp));
  func= &eq_creator;
  init(select_lex, new (thd->mem_root) select_exists_subselect(thd, this));
  max_columns= UINT_MAX;
  set_maybe_null();
  reset();
  //if test_limit will fail then error will be reported to client
  test_limit(select_lex->master_unit());
  DBUG_VOID_RETURN;
}

int Item_in_subselect::get_identifier()
{
  return engine->get_identifier();
}

Item_allany_subselect::Item_allany_subselect(THD *thd, Item * left_exp,
                                             chooser_compare_func_creator fc,
					     st_select_lex *select_lex,
					     bool all_arg):
  Item_in_subselect(thd), func_creator(fc), all(all_arg)
{
  DBUG_ENTER("Item_allany_subselect::Item_allany_subselect");
  left_expr_orig= left_expr= left_exp;
  /* prepare to possible disassembling the item in convert_subq_to_sj() */
  if (left_exp->type() == Item::ROW_ITEM)
    left_expr_orig= new (thd->mem_root)
      Item_row(thd, static_cast<Item_row*>(left_exp));
  func= func_creator(all_arg);
  init(select_lex, new (thd->mem_root) select_exists_subselect(thd, this));
  max_columns= 1;
  reset();
  //if test_limit will fail then error will be reported to client
  test_limit(select_lex->master_unit());
  DBUG_VOID_RETURN;
}


/**
  Initialize length and decimals for EXISTS  and inherited (IN/ALL/ANY)
  subqueries
*/

void Item_exists_subselect::init_length_and_dec()
{
  decimals= 0;
  max_length= 1;
  max_columns= engine->cols();
}


bool Item_exists_subselect::fix_length_and_dec()
{
  DBUG_ENTER("Item_exists_subselect::fix_length_and_dec");
  init_length_and_dec();
  // If limit is not set or it is constant more than 1
  if (!unit->global_parameters()->limit_params.select_limit ||
      (unit->global_parameters()->limit_params.select_limit->basic_const_item() &&
       unit->global_parameters()->limit_params.select_limit->val_int() > 1))
  {
    /*
       We need only 1 row to determine existence (i.e. any EXISTS that is not
       an IN always requires LIMIT 1)
     */
    Item *item= new (thd->mem_root) Item_int(thd, (int32) 1);
    if (!item)
      DBUG_RETURN(TRUE);
    thd->change_item_tree(&unit->global_parameters()->limit_params.select_limit,
                          item);
    unit->global_parameters()->limit_params.explicit_limit= 1; // we set the limit
    DBUG_PRINT("info", ("Set limit to 1"));
  }
  DBUG_RETURN(FALSE);
}


bool Item_in_subselect::fix_length_and_dec()
{
  DBUG_ENTER("Item_in_subselect::fix_length_and_dec");
  init_length_and_dec();
  /*
    Unlike Item_exists_subselect, LIMIT 1 is set later for
    Item_in_subselect, depending on the chosen strategy.
  */
  DBUG_RETURN(FALSE);
}


/**
  Add an expression cache for this subquery if it is needed

  @param thd_arg         Thread handle

  @details
  The function checks whether an expression cache is needed for this item
  and if if so wraps the item into an item of the class
  Item_cache_wrapper with an appropriate expression cache set up there.

  @note
  used from Item::transform()

  @return
  new wrapper item if an expression cache is needed,
  this item - otherwise
*/

Item* Item_exists_subselect::expr_cache_insert_transformer(THD *tmp_thd,
                                                           uchar *unused)
{
  DBUG_ENTER("Item_exists_subselect::expr_cache_insert_transformer");
  DBUG_ASSERT(thd == tmp_thd);

  if (expr_cache)
    DBUG_RETURN(expr_cache);

  if (substype() == EXISTS_SUBS && expr_cache_is_needed(tmp_thd) &&
      (expr_cache= set_expr_cache(tmp_thd)))
  {
    init_expr_cache_tracker(tmp_thd);
    DBUG_RETURN(expr_cache);
  }
  DBUG_RETURN(this);
}


void Item_exists_subselect::no_rows_in_result()
{
  /*
    Subquery predicates outside of the SELECT list must be evaluated in order
    to possibly filter the special result row generated for implicit grouping
    if the subquery is in the HAVING clause.
    If the predicate is constant, we need its actual value in the only result
    row for queries with implicit grouping.
  */
  if (parsing_place != SELECT_LIST || const_item())
    return;
  value= 0;
  null_value= 0;
  make_const();
}

double Item_exists_subselect::val_real()
{
  DBUG_ASSERT(fixed());
  if (!forced_const && exec())
  {
    reset();
    return 0;
  }
  return (double) value;
}

longlong Item_exists_subselect::val_int()
{
  DBUG_ASSERT(fixed());
  if (!forced_const && exec())
  {
    reset();
    return 0;
  }
  return value;
}


/**
  Return the result of EXISTS as a string value

  Converts the true/false result into a string value.
  Note that currently this cannot be NULL, so if the query execution fails
  it will return 0.

  @param decimal_value[out]    buffer to hold the resulting string value
  @retval                      Pointer to the converted string.
                               Can't be a NULL pointer, as currently
                               EXISTS cannot return NULL.
*/

String *Item_exists_subselect::val_str(String *str)
{
  DBUG_ASSERT(fixed());
  if (!forced_const && exec())
    reset();
  str->set((ulonglong)value,&my_charset_bin);
  return str;
}


/**
  Return the result of EXISTS as a decimal value

  Converts the true/false result into a decimal value.
  Note that currently this cannot be NULL, so if the query execution fails
  it will return 0.

  @param decimal_value[out]    Buffer to hold the resulting decimal value
  @retval                      Pointer to the converted decimal.
                               Can't be a NULL pointer, as currently
                               EXISTS cannot return NULL.
*/

my_decimal *Item_exists_subselect::val_decimal(my_decimal *decimal_value)
{
  DBUG_ASSERT(fixed());
  if (!forced_const && exec())
    reset();
  int2my_decimal(E_DEC_FATAL_ERROR, value, 0, decimal_value);
  return decimal_value;
}


bool Item_exists_subselect::val_bool()
{
  DBUG_ASSERT(fixed());
  if (!forced_const && exec())
  {
    reset();
    return 0;
  }
  return value != 0;
}


double Item_in_subselect::val_real()
{
  /*
    As far as Item_in_subselect called only from Item_in_optimizer this
    method should not be used
  */
  DBUG_ASSERT(fixed());
  if (forced_const)
    return value;
  DBUG_ASSERT((engine->uncacheable() & ~UNCACHEABLE_EXPLAIN) ||
              ! engine->is_executed());
  null_value= was_null= FALSE;
  if (exec())
  {
    reset();
    return 0;
  }
  if (was_null && !value)
    null_value= TRUE;
  return (double) value;
}


longlong Item_in_subselect::val_int()
{
  /*
    As far as Item_in_subselect called only from Item_in_optimizer this
    method should not be used
  */
  DBUG_ASSERT(0);
  DBUG_ASSERT(fixed());
  if (forced_const)
    return value;
  DBUG_ASSERT((engine->uncacheable() & ~UNCACHEABLE_EXPLAIN) ||
              ! engine->is_executed());
  null_value= was_null= FALSE;
  if (exec())
  {
    reset();
    return 0;
  }
  if (was_null && !value)
    null_value= TRUE;
  return value;
}


String *Item_in_subselect::val_str(String *str)
{
  /*
    As far as Item_in_subselect called only from Item_in_optimizer this
    method should not be used
  */
  DBUG_ASSERT(0);
  DBUG_ASSERT(fixed());
  if (forced_const)
    goto value_is_ready;
  DBUG_ASSERT((engine->uncacheable() & ~UNCACHEABLE_EXPLAIN) ||
              ! engine->is_executed());
  null_value= was_null= FALSE;
  if (exec())
  {
    reset();
    return 0;
  }
  if (was_null && !value)
  {
    null_value= TRUE;
    return 0;
  }
value_is_ready:
  str->set((ulonglong)value, &my_charset_bin);
  return str;
}


bool Item_in_subselect::val_bool()
{
  DBUG_ASSERT(fixed());
  if (forced_const)
    return value;
  DBUG_ASSERT((engine->uncacheable() & ~UNCACHEABLE_EXPLAIN) ||
              ! engine->is_executed() || with_recursive_reference);
  null_value= was_null= FALSE;
  if (exec())
  {
    reset();
    return 0;
  }
  if (was_null && !value)
    null_value= TRUE;
  return value;
}

my_decimal *Item_in_subselect::val_decimal(my_decimal *decimal_value)
{
  /*
    As far as Item_in_subselect called only from Item_in_optimizer this
    method should not be used
  */
  DBUG_ASSERT(0);
  if (forced_const)
    goto value_is_ready;
  DBUG_ASSERT((engine->uncacheable() & ~UNCACHEABLE_EXPLAIN) ||
              ! engine->is_executed());
  null_value= was_null= FALSE;
  DBUG_ASSERT(fixed());
  if (exec())
  {
    reset();
    return 0;
  }
  if (was_null && !value)
    null_value= TRUE;
value_is_ready:
  int2my_decimal(E_DEC_FATAL_ERROR, value, 0, decimal_value);
  return decimal_value;
}


/**
  Prepare a single-column IN/ALL/ANY subselect for rewriting.

  @param join  Join object of the subquery (i.e. 'child' join).

  @details

  Prepare a single-column subquery to be rewritten. Given the subquery.

  If the subquery has no tables it will be turned to an expression between
  left part and SELECT list.

  In other cases the subquery will be wrapped with  Item_in_optimizer which
  allow later to turn it to EXISTS or MAX/MIN.

  @retval false  The subquery was transformed
  @retval true   Error
*/

bool
Item_in_subselect::single_value_transformer(JOIN *join)
{
  SELECT_LEX *select_lex= join->select_lex;
  DBUG_ENTER("Item_in_subselect::single_value_transformer");
  DBUG_ASSERT(thd == join->thd);

  /*
    Check that the right part of the subselect contains no more than one
    column. E.g. in SELECT 1 IN (SELECT * ..) the right part is (SELECT * ...)
  */
  // psergey: duplicated_subselect_card_check
  if (select_lex->item_list.elements > 1)
  {
    my_error(ER_OPERAND_COLUMNS, MYF(0), 1);
    DBUG_RETURN(true);
  }

  Item* join_having= join->having ? join->having : join->tmp_having;
  if (!(join_having || select_lex->with_sum_func ||
        select_lex->group_list.elements) &&
      select_lex->table_list.elements == 0 && !join->conds &&
      !select_lex->master_unit()->is_unit_op())
  {
    Item *where_item= (Item*) select_lex->item_list.head();
    /*
      it is single select without tables => possible optimization
      remove the dependence mark since the item is moved to upper
      select and is not outer anymore.
    */
    where_item->walk(&Item::remove_dependence_processor, 0,
                     select_lex->outer_select());
    /*
      fix_field of substitution item will be done in time of
      substituting.
      Note that real_item() should be used instead of
      original left expression because left_expr can be
      runtime created Ref item which is deleted at the end
      of the statement. Thus one of 'substitution' arguments
      can be broken in case of PS.
    */ 
    substitution= func->create(thd, left_expr, where_item);
    have_to_be_excluded= 1;
    if (thd->lex->describe)
    {
      char warn_buff[MYSQL_ERRMSG_SIZE];
      sprintf(warn_buff, ER_THD(thd, ER_SELECT_REDUCED),
              select_lex->select_number);
      push_warning(thd, Sql_condition::WARN_LEVEL_NOTE,
                   ER_SELECT_REDUCED, warn_buff);
    }
    DBUG_RETURN(false);
  }

  /*
    Wrap the current IN predicate in an Item_in_optimizer. The actual
    substitution in the Item tree takes place in Item_subselect::fix_fields.
  */
  if (!substitution)
  {
    /* We're invoked for the 1st (or the only) SELECT in the subquery UNION */
    substitution= optimizer;

    SELECT_LEX *current= thd->lex->current_select;

    thd->lex->current_select= current->return_after_parsing();
    if (!optimizer || optimizer->fix_left(thd))
    {
      thd->lex->current_select= current;
      DBUG_RETURN(true);
    }
    thd->lex->current_select= current;

    /* We will refer to upper level cache array => we have to save it for SP */
    DBUG_ASSERT(optimizer->get_cache()[0]->is_array_kept());

    /*
      As far as  Item_in_optimizer does not substitute itself on fix_fields
      we can use same item for all selects.
    */
    expr= new (thd->mem_root) Item_direct_ref(thd, &select_lex->context,
                              (Item**)optimizer->get_cache(),
                              no_matter_name,
			      in_left_expr_name);
  }

  DBUG_RETURN(false);
}


/**
  Apply transformation max/min  transwormation to ALL/ANY subquery if it is
  possible.

  @param join  Join object of the subquery (i.e. 'child' join).

  @details

  If this is an ALL/ANY single-value subselect, try to rewrite it with
  a MIN/MAX subselect. We can do that if a possible NULL result of the
  subselect can be ignored.
  E.g. SELECT * FROM t1 WHERE b > ANY (SELECT a FROM t2) can be rewritten
  with SELECT * FROM t1 WHERE b > (SELECT MAX(a) FROM t2).
  We can't check that this optimization is safe if it's not a top-level
  item of the WHERE clause (e.g. because the WHERE clause can contain IS
  NULL/IS NOT NULL functions). If so, we rewrite ALL/ANY with NOT EXISTS
  later in this method.

  @retval false  The subquery was transformed
  @retval true   Error
*/

bool Item_allany_subselect::transform_into_max_min(JOIN *join)
{
  DBUG_ENTER("Item_allany_subselect::transform_into_max_min");
  if (!test_strategy(SUBS_MAXMIN_INJECTED | SUBS_MAXMIN_ENGINE))
    DBUG_RETURN(false);
  Item **place= optimizer->arguments() + 1;
  SELECT_LEX *select_lex= join->select_lex;
  Item *subs;
  DBUG_ASSERT(thd == join->thd);

  /*
  */
  DBUG_ASSERT(!substitution);

  /*
    Check if optimization with aggregate min/max possible
    1 There is no aggregate in the subquery
    2 It is not UNION
    3 There is tables
    4 It is not ALL subquery with possible NULLs in the SELECT list
  */
  if (!select_lex->group_list.elements &&                 /*1*/
      !select_lex->having &&                              /*1*/
      !select_lex->with_sum_func &&                       /*1*/
      !(select_lex->next_select()) &&                     /*2*/
      select_lex->table_list.elements &&                  /*3*/
      (!select_lex->ref_pointer_array[0]->maybe_null() || /*4*/
       substype() != Item_subselect::ALL_SUBS))           /*4*/
  {
    Item_sum_min_max *item;
    nesting_map save_allow_sum_func;
    if (func->l_op())
    {
      /*
        (ALL && (> || =>)) || (ANY && (< || =<))
        for ALL condition is inverted
      */
      item= new (thd->mem_root) Item_sum_max(thd,
                                             select_lex->ref_pointer_array[0]);
    }
    else
    {
      /*
        (ALL && (< || =<)) || (ANY && (> || =>))
        for ALL condition is inverted
      */
      item= new (thd->mem_root) Item_sum_min(thd,
                                             select_lex->ref_pointer_array[0]);
    }
    if (upper_item)
      upper_item->set_sum_test(item);
    thd->change_item_tree(&select_lex->ref_pointer_array[0], item);
    {
      List_iterator<Item> it(select_lex->item_list);
      it++;
      thd->change_item_tree(it.ref(), item);
    }

    DBUG_EXECUTE("where",
                 print_where(item, "rewrite with MIN/MAX", QT_ORDINARY););

    save_allow_sum_func= thd->lex->allow_sum_func;
    thd->lex->allow_sum_func.set_bit(thd->lex->current_select->nest_level);
    /*
      Item_sum_(max|min) can't substitute other item => we can use 0 as
      reference, also Item_sum_(max|min) can't be fixed after creation, so
      we do not check item->fixed
    */
    if (item->fix_fields(thd, 0))
      DBUG_RETURN(true);
    thd->lex->allow_sum_func= save_allow_sum_func; 
    /* we added aggregate function => we have to change statistic */
    count_field_types(select_lex, &join->tmp_table_param, join->all_fields, 
                      0);
    if (join->prepare_stage2())
      DBUG_RETURN(true);
    subs= new (thd->mem_root) Item_singlerow_subselect(thd, select_lex);

    /*
      Remove other strategies if any (we already changed the query and
      can't apply other strategy).
    */
    set_strategy(SUBS_MAXMIN_INJECTED);
  }
  else
  {
    Item_maxmin_subselect *item;
    subs= item= new (thd->mem_root) Item_maxmin_subselect(thd, this, select_lex, func->l_op());
    if (upper_item)
      upper_item->set_sub_test(item);
    /*
      Remove other strategies if any (we already changed the query and
      can't apply other strategy).
    */
    set_strategy(SUBS_MAXMIN_ENGINE);
  }
  /*
    The swap is needed for expressions of type 'f1 < ALL ( SELECT ....)'
    where we want to evaluate the sub query even if f1 would be null.
  */
  subs= func->create_swap(thd, expr, subs);
  thd->change_item_tree(place, subs);
  if (subs->fix_fields(thd, &subs))
    DBUG_RETURN(true);
  DBUG_ASSERT(subs == (*place)); // There was no substitutions

  select_lex->master_unit()->uncacheable&= ~UNCACHEABLE_DEPENDENT_INJECTED;
  select_lex->uncacheable&= ~UNCACHEABLE_DEPENDENT_INJECTED;

  DBUG_RETURN(false);
}


bool Item_in_subselect::fix_having(Item *having, SELECT_LEX *select_lex)
{
  bool fix_res= 0;
  DBUG_ASSERT(thd);
  if (!having->fixed())
  {
    select_lex->having_fix_field= 1;
    fix_res= having->fix_fields(thd, 0);
    select_lex->having_fix_field= 0;
  }
  return fix_res;
}

bool Item_allany_subselect::is_maxmin_applicable(JOIN *join)
{
  /*
    Check if max/min optimization applicable: It is top item of
    WHERE condition.
  */
  return ((is_top_level_item() ||
           (upper_item && upper_item->is_top_level_item())) &&
          !(join->select_lex->master_unit()->uncacheable &
            ~UNCACHEABLE_EXPLAIN) &&
          !func->eqne_op());
}


/**
  Create the predicates needed to transform a single-column IN/ALL/ANY
  subselect into a correlated EXISTS via predicate injection.

  @param join[in]  Join object of the subquery (i.e. 'child' join).
  @param where_item[out]   the in-to-exists addition to the where clause
  @param having_item[out]  the in-to-exists addition to the having clause

  @details
  The correlated predicates are created as follows:

  - If the subquery has aggregates, GROUP BY, or HAVING, convert to

    SELECT ie FROM ...  HAVING subq_having AND 
                               trigcond(oe $cmp$ ref_or_null_helper<ie>)
                                   
    the addition is wrapped into trigger only when we want to distinguish
    between NULL and FALSE results.

  - Otherwise (no aggregates/GROUP BY/HAVING) convert it to one of the
    following:

    = If we don't need to distinguish between NULL and FALSE subquery:
        
      SELECT ie FROM ... WHERE subq_where AND (oe $cmp$ ie)

    = If we need to distinguish between those:

      SELECT ie FROM ...
        WHERE  subq_where AND trigcond((oe $cmp$ ie) OR (ie IS NULL))
        HAVING trigcond(<is_not_null_test>(ie))

  @retval false If the new conditions were created successfully
  @retval true  Error
*/

bool
Item_in_subselect::create_single_in_to_exists_cond(JOIN *join,
                                                   Item **where_item,
                                                   Item **having_item)
{
  SELECT_LEX *select_lex= join->select_lex;
  DBUG_ASSERT(thd == join->thd);
  /*
    The non-transformed HAVING clause of 'join' may be stored in two ways
    during JOIN::optimize: this->tmp_having= this->having; this->having= 0;
  */
  Item* join_having= join->having ? join->having : join->tmp_having;
  DBUG_ENTER("Item_in_subselect::create_single_in_to_exists_cond");

  *where_item= NULL;
  *having_item= NULL;

  if (join_having || select_lex->with_sum_func ||
      select_lex->group_list.elements)
  {
    LEX_CSTRING field_name= this->full_name_cstring();
    Item *item= func->create(thd, expr,
                             new (thd->mem_root) Item_ref_null_helper(
                                                      thd,
                                                      &select_lex->context,
                                                      this,
                                                      &select_lex->
                                                      ref_pointer_array[0],  
                                                      {STRING_WITH_LEN("<ref>")},
                                                      field_name));
    if (!is_top_level_item() && left_expr->maybe_null())
    {
      /* 
        We can encounter "NULL IN (SELECT ...)". Wrap the added condition
        within a trig_cond.
      */
      disable_cond_guard_for_const_null_left_expr(0);
      item= new (thd->mem_root) Item_func_trig_cond(thd, item, get_cond_guard(0));
    }

    if (!join_having)
      item->name= in_having_cond;
    if (fix_having(item, select_lex))
      DBUG_RETURN(true);
    *having_item= item;
  }
  else
  {
    /*
      No need to use real_item for the item, as the ref items that are possible
      in the subquery either belong to views or to the parent select.
      For such case we need to refer to the reference and not to the original
      item.
    */
    Item *item= (Item*) select_lex->item_list.head();

    if (select_lex->table_list.elements ||
        !(select_lex->master_unit()->is_unit_op()))
    {
      Item *having= item;
      Item *orig_item= item;

      item= func->create(thd, expr, item);
      if (!is_top_level_item() && orig_item->maybe_null())
      {
	having= new (thd->mem_root) Item_is_not_null_test(thd, this, having);
        if (left_expr->maybe_null())
        {
          disable_cond_guard_for_const_null_left_expr(0);
          if (!(having= new (thd->mem_root) Item_func_trig_cond(thd, having,
                                                            get_cond_guard(0))))
            DBUG_RETURN(true);
        }
        having->name= in_having_cond;
        if (fix_having(having, select_lex))
          DBUG_RETURN(true);
        *having_item= having;

	item= new (thd->mem_root) Item_cond_or(thd, item,
                               new (thd->mem_root) Item_func_isnull(thd, orig_item));
      }
      /* 
        If we may encounter NULL IN (SELECT ...) and care whether subquery
        result is NULL or FALSE, wrap condition in a trig_cond.
      */
      if (!is_top_level_item() && left_expr->maybe_null())
      {
        disable_cond_guard_for_const_null_left_expr(0);
        if (!(item= new (thd->mem_root) Item_func_trig_cond(thd, item,
                                                            get_cond_guard(0))))
          DBUG_RETURN(true);
      }

      /*
        TODO: figure out why the following is done here in 
        single_value_transformer but there is no corresponding action in
        row_value_transformer?
      */
      item->name= in_additional_cond;
      if (item->fix_fields_if_needed(thd, 0))
        DBUG_RETURN(true);
      *where_item= item;
    }
    else
    {
      DBUG_ASSERT(select_lex->master_unit()->is_unit_op());
      LEX_CSTRING field_name= {STRING_WITH_LEN("<result>") };
      Item *new_having=
        func->create(thd, expr,
                     new (thd->mem_root) Item_ref_null_helper(thd,
                                                  &select_lex->context,
                                                  this,
                                                  &select_lex->ref_pointer_array[0],
                                                  no_matter_name,
                                                  field_name));
      if (!is_top_level_item() && left_expr->maybe_null())
      {
        disable_cond_guard_for_const_null_left_expr(0);
        if (!(new_having= new (thd->mem_root)
              Item_func_trig_cond(thd, new_having, get_cond_guard(0))))
          DBUG_RETURN(true);
      }

      new_having->name= in_having_cond;
      if (fix_having(new_having, select_lex))
        DBUG_RETURN(true);

      *having_item= new_having;
    }
  }

  DBUG_RETURN(false);
}


/**
  Wrap a multi-column IN/ALL/ANY subselect into an Item_in_optimizer.

  @param join  Join object of the subquery (i.e. 'child' join).

  @details
  The subquery predicate is wrapped into an Item_in_optimizer. Later the query
  optimization phase chooses whether the subquery under the Item_in_optimizer
  will be further transformed into an equivalent correlated EXISTS by injecting
  additional predicates, or will be executed via subquery materialization in its
  unmodified form.

  @retval false  The subquery was transformed
  @retval true   Error
*/

bool
Item_in_subselect::row_value_transformer(JOIN *join)
{
  SELECT_LEX *select_lex= join->select_lex;
  uint cols_num= left_expr->cols();

  DBUG_ENTER("Item_in_subselect::row_value_transformer");
  DBUG_ASSERT(thd == join->thd);

  // psergey: duplicated_subselect_card_check
  if (select_lex->item_list.elements != cols_num)
  {
    my_error(ER_OPERAND_COLUMNS, MYF(0), cols_num);
    DBUG_RETURN(true);
  }

  /*
    Wrap the current IN predicate in an Item_in_optimizer. The actual
    substitution in the Item tree takes place in Item_subselect::fix_fields.
  */
  if (!substitution)
  {
    //first call for this unit
    SELECT_LEX_UNIT *master_unit= select_lex->master_unit();
    substitution= optimizer;

    SELECT_LEX *current= thd->lex->current_select;
    thd->lex->current_select= current->return_after_parsing();
    if (!optimizer || optimizer->fix_left(thd))
    {
      thd->lex->current_select= current;
      DBUG_RETURN(true);
    }

    // we will refer to upper level cache array => we have to save it in PS
    DBUG_ASSERT(optimizer->get_cache()[0]->is_array_kept());

    thd->lex->current_select= current;
    /*
      The uncacheable property controls a number of actions, e.g. whether to
      save/restore (via init_save_join_tab/restore_tmp) the original JOIN for
      plans with a temp table where the original JOIN was overridden by
      make_simple_join. The UNCACHEABLE_EXPLAIN is ignored by EXPLAIN, thus
      non-correlated subqueries will not appear as such to EXPLAIN.
    */
    master_unit->uncacheable|= UNCACHEABLE_EXPLAIN;
    select_lex->uncacheable|= UNCACHEABLE_EXPLAIN;
  }

  DBUG_RETURN(false);
}


/**
  Create the predicates needed to transform a multi-column IN/ALL/ANY
  subselect into a correlated EXISTS via predicate injection.

  @details
  The correlated predicates are created as follows:

  - If the subquery has aggregates, GROUP BY, or HAVING, convert to

    (l1, l2, l3) IN (SELECT v1, v2, v3 ... HAVING having)
    =>
    EXISTS (SELECT ... HAVING having and
                              (l1 = v1 or is null v1) and
                              (l2 = v2 or is null v2) and
                              (l3 = v3 or is null v3) and
                              is_not_null_test(v1) and
                              is_not_null_test(v2) and
                              is_not_null_test(v3))

    where is_not_null_test used to register nulls in case if we have
    not found matching to return correct NULL value.

  - Otherwise (no aggregates/GROUP BY/HAVING) convert the subquery as follows:

    (l1, l2, l3) IN (SELECT v1, v2, v3 ... WHERE where)
    =>
    EXISTS (SELECT ... WHERE where and
                             (l1 = v1 or is null v1) and
                             (l2 = v2 or is null v2) and
                             (l3 = v3 or is null v3)
                       HAVING is_not_null_test(v1) and
                              is_not_null_test(v2) and
                              is_not_null_test(v3))
    where is_not_null_test registers NULLs values but reject rows.

    in case when we do not need correct NULL, we have simpler construction:
    EXISTS (SELECT ... WHERE where and
                             (l1 = v1) and
                             (l2 = v2) and
                             (l3 = v3)

  @param join[in]  Join object of the subquery (i.e. 'child' join).
  @param where_item[out]   the in-to-exists addition to the where clause
  @param having_item[out]  the in-to-exists addition to the having clause

  @retval false  If the new conditions were created successfully
  @retval true   Error
*/

bool
Item_in_subselect::create_row_in_to_exists_cond(JOIN * join,
                                                Item **where_item,
                                                Item **having_item)
{
  SELECT_LEX *select_lex= join->select_lex;
  uint cols_num= left_expr->cols();
  /*
    The non-transformed HAVING clause of 'join' may be stored in two ways
    during JOIN::optimize: this->tmp_having= this->having; this->having= 0;
  */
  Item* join_having= join->having ? join->having : join->tmp_having;
  bool is_having_used= (join_having || select_lex->with_sum_func ||
                        select_lex->group_list.first ||
                        !select_lex->table_list.elements);
  LEX_CSTRING list_ref= { STRING_WITH_LEN("<list ref>")};
  DBUG_ENTER("Item_in_subselect::create_row_in_to_exists_cond");
  DBUG_ASSERT(thd == join->thd);

  *where_item= NULL;
  *having_item= NULL;

  if (is_having_used)
  {
    /* TODO: say here explicitly if the order of AND parts matters or not. */
    Item *item_having_part2= 0;
    for (uint i= 0; i < cols_num; i++)
    {
      DBUG_ASSERT((left_expr->fixed() &&

                  select_lex->ref_pointer_array[i]->fixed()) ||
                  (select_lex->ref_pointer_array[i]->type() == REF_ITEM &&
                   ((Item_ref*)(select_lex->ref_pointer_array[i]))->ref_type() ==
                    Item_ref::OUTER_REF));
      Item *item_eq=
        new (thd->mem_root)
        Item_func_eq(thd, new (thd->mem_root)
                     Item_direct_ref(thd, &select_lex->context,
                                     (*optimizer->get_cache())->
                                     addr(i),
                                     no_matter_name,
                                     in_left_expr_name),
                     new (thd->mem_root)
                     Item_ref(thd, &select_lex->context,
                              &select_lex->ref_pointer_array[i],
                              no_matter_name,
                              list_ref));
      Item *item_isnull=
        new (thd->mem_root)
        Item_func_isnull(thd,
                         new (thd->mem_root)
                         Item_ref(thd, &select_lex->context,
                                  &select_lex->ref_pointer_array[i],
                                  no_matter_name,
                                  list_ref));
      Item *col_item= new (thd->mem_root)
        Item_cond_or(thd, item_eq, item_isnull);
      if (!is_top_level_item() && left_expr->element_index(i)->maybe_null() &&
          get_cond_guard(i))
      {
        disable_cond_guard_for_const_null_left_expr(i);
        if (!(col_item= new (thd->mem_root)
              Item_func_trig_cond(thd, col_item, get_cond_guard(i))))
          DBUG_RETURN(true);
      }
      *having_item= and_items(thd, *having_item, col_item);

      Item *item_nnull_test= 
         new (thd->mem_root)
        Item_is_not_null_test(thd, this,
                              new (thd->mem_root)
                              Item_ref(thd, &select_lex->context,
                                       &select_lex->
                                       ref_pointer_array[i],
                                       no_matter_name,
                                       list_ref));
      if (!is_top_level_item() && left_expr->element_index(i)->maybe_null() &&
          get_cond_guard(i) )
      {
        disable_cond_guard_for_const_null_left_expr(i);
        if (!(item_nnull_test= 
              new (thd->mem_root)
              Item_func_trig_cond(thd, item_nnull_test, get_cond_guard(i))))
          DBUG_RETURN(true);
      }
      item_having_part2= and_items(thd, item_having_part2, item_nnull_test);
      item_having_part2->top_level_item();
    }
    *having_item= and_items(thd, *having_item, item_having_part2);
  }
  else
  {
    for (uint i= 0; i < cols_num; i++)
    {
      Item *item, *item_isnull;
      DBUG_ASSERT((left_expr->fixed() &&
                  select_lex->ref_pointer_array[i]->fixed()) ||
                  (select_lex->ref_pointer_array[i]->type() == REF_ITEM &&
                   ((Item_ref*)(select_lex->ref_pointer_array[i]))->ref_type() ==
                    Item_ref::OUTER_REF));
      item= new (thd->mem_root)
        Item_func_eq(thd,
                     new (thd->mem_root)
                     Item_direct_ref(thd, &select_lex->context,
                                     (*optimizer->get_cache())->
                                     addr(i),
                                     no_matter_name,
                                     in_left_expr_name),
                     new (thd->mem_root)
                     Item_direct_ref(thd, &select_lex->context,
                                     &select_lex->
                                     ref_pointer_array[i],
                                     no_matter_name,
                                     list_ref));
      if (!is_top_level_item() && select_lex->ref_pointer_array[i]->maybe_null())
      {
        Item *having_col_item=
          new (thd->mem_root)
          Item_is_not_null_test(thd, this,
                                new (thd->mem_root)
                                Item_ref(thd, &select_lex->context, 
                                         &select_lex->ref_pointer_array[i],
                                         no_matter_name,
                                         list_ref));
        
        item_isnull= new (thd->mem_root)
          Item_func_isnull(thd,
                           new (thd->mem_root)
                           Item_direct_ref(thd, &select_lex->context,
                                           &select_lex->
                                           ref_pointer_array[i],
                                           no_matter_name,
                                           list_ref));
        item= new (thd->mem_root) Item_cond_or(thd, item, item_isnull);
        if (left_expr->element_index(i)->maybe_null() && get_cond_guard(i))
        {
          disable_cond_guard_for_const_null_left_expr(i);
          if (!(item= new (thd->mem_root)
                Item_func_trig_cond(thd, item, get_cond_guard(i))))
            DBUG_RETURN(true);
          if (!(having_col_item= new (thd->mem_root)
                Item_func_trig_cond(thd, having_col_item, get_cond_guard(i))))
            DBUG_RETURN(true);
        }
        *having_item= and_items(thd, *having_item, having_col_item);
      }
      if (!is_top_level_item() && left_expr->element_index(i)->maybe_null() &&
          get_cond_guard(i))
      {
        if (!(item= new (thd->mem_root)
              Item_func_trig_cond(thd, item, get_cond_guard(i))))
          DBUG_RETURN(true);
      }
      *where_item= and_items(thd, *where_item, item);
    }
  }

  if (*where_item)
  {
    if ((*where_item)->fix_fields_if_needed(thd, 0))
      DBUG_RETURN(true);
    (*where_item)->top_level_item();
  }

  if (*having_item)
  {
    if (!join_having)
      (*having_item)->name= in_having_cond;
    if (fix_having(*having_item, select_lex))
      DBUG_RETURN(true);
    (*having_item)->top_level_item();
  }

  DBUG_RETURN(false);
}


bool
Item_in_subselect::select_transformer(JOIN *join)
{
  return select_in_like_transformer(join);
}

bool
Item_exists_subselect::select_transformer(JOIN *join)
{
  return select_prepare_to_be_in();
}


/**
  Create the predicates needed to transform an IN/ALL/ANY subselect into a
  correlated EXISTS via predicate injection.

  @param join_arg  Join object of the subquery.

  @retval FALSE  ok
  @retval TRUE   error
*/

bool Item_in_subselect::create_in_to_exists_cond(JOIN *join_arg)
{
  bool res;

  DBUG_ASSERT(engine->engine_type() == subselect_engine::SINGLE_SELECT_ENGINE ||
              engine->engine_type() == subselect_engine::UNION_ENGINE);
  /*
    TODO: the call to init_cond_guards allocates and initializes an
    array of booleans that may not be used later because we may choose
    materialization.
    The two calls below to create_XYZ_cond depend on this boolean array.
    If the dependency is removed, the call can be moved to a later phase.
  */
  init_cond_guards();
  if (left_expr->cols() == 1)
    res= create_single_in_to_exists_cond(join_arg,
                                         &(join_arg->in_to_exists_where),
                                         &(join_arg->in_to_exists_having));
  else
    res= create_row_in_to_exists_cond(join_arg,
                                      &(join_arg->in_to_exists_where),
                                      &(join_arg->in_to_exists_having));

  /*
    The IN=>EXISTS transformation makes non-correlated subqueries correlated.
  */
  if (!left_expr->can_eval_in_optimize())
  {
    join_arg->select_lex->uncacheable|= UNCACHEABLE_DEPENDENT_INJECTED;
    join_arg->select_lex->master_unit()->uncacheable|= 
                                         UNCACHEABLE_DEPENDENT_INJECTED;
  }
  /*
    The uncacheable property controls a number of actions, e.g. whether to
    save/restore (via init_save_join_tab/restore_tmp) the original JOIN for
    plans with a temp table where the original JOIN was overridden by
    make_simple_join. The UNCACHEABLE_EXPLAIN is ignored by EXPLAIN, thus
    non-correlated subqueries will not appear as such to EXPLAIN.
  */
  join_arg->select_lex->master_unit()->uncacheable|= UNCACHEABLE_EXPLAIN;
  join_arg->select_lex->uncacheable|= UNCACHEABLE_EXPLAIN;
  return (res);
}


/**
  Transform an IN/ALL/ANY subselect into a correlated EXISTS via injecting
  correlated in-to-exists predicates.

  @param join_arg  Join object of the subquery.

  @retval FALSE  ok
  @retval TRUE   error
*/

bool Item_in_subselect::inject_in_to_exists_cond(JOIN *join_arg)
{
  SELECT_LEX *select_lex= join_arg->select_lex;
  Item *where_item= join_arg->in_to_exists_where;
  Item *having_item= join_arg->in_to_exists_having;

  DBUG_ENTER("Item_in_subselect::inject_in_to_exists_cond");
  DBUG_ASSERT(thd == join_arg->thd);

  if (select_lex->min_max_opt_list.elements)
  {
    /*
      MIN/MAX optimizations have been applied to Item_sum objects
      of the subquery this subquery predicate in opt_sum_query().
      Injection of new condition invalidates this optimizations.
      Thus those optimizations must be rolled back.
    */
    List_iterator_fast<Item_sum> it(select_lex->min_max_opt_list);
    Item_sum *item;
    while ((item= it++))
    {
      item->clear();
      item->reset_forced_const();
    }
    if (where_item)
      where_item->update_used_tables();
    if (having_item)
      having_item->update_used_tables();
  }

  if (where_item)
  {
    List<Item> *and_args= NULL;
    /*
      If the top-level Item of the WHERE clause is an AND, detach the multiple
      equality list that was attached to the end of the AND argument list by
      build_equal_items_for_cond(). The multiple equalities must be detached
      because fix_fields merges lower level AND arguments into the upper AND.
      As a result, the arguments from lower-level ANDs are concatenated after
      the multiple equalities. When the multiple equality list is treated as
      such, it turns out that it contains non-Item_equal object which is wrong.
    */
    if (join_arg->conds && join_arg->conds->type() == Item::COND_ITEM &&
        ((Item_cond*) join_arg->conds)->functype() == Item_func::COND_AND_FUNC)
    {
      and_args= ((Item_cond*) join_arg->conds)->argument_list();
      if (join_arg->cond_equal)
        and_args->disjoin((List<Item> *) &join_arg->cond_equal->current_level);
    }

    where_item= and_items(thd, join_arg->conds, where_item);

    /* This is the fix_fields() call mentioned in the comment above */
    if (where_item->fix_fields_if_needed(thd, 0))
      DBUG_RETURN(true);
    // TIMOUR TODO: call optimize_cond() for the new where clause
    thd->change_item_tree(&select_lex->where, where_item);
    select_lex->where->top_level_item();
    join_arg->conds= select_lex->where;

    /* Attach back the list of multiple equalities to the new top-level AND. */
    if (and_args && join_arg->cond_equal)
    {
      /*
        The fix_fields() call above may have changed the argument list, so
        fetch it again:
      */
      and_args= ((Item_cond*) join_arg->conds)->argument_list();
      ((Item_cond_and *) (join_arg->conds))->m_cond_equal=
                                             *join_arg->cond_equal;
      and_args->append((List<Item> *)&join_arg->cond_equal->current_level);
    }
  }

  if (having_item)
  {
    Item* join_having= join_arg->having ? join_arg->having:join_arg->tmp_having;
    having_item= and_items(thd, join_having, having_item);
    if (fix_having(having_item, select_lex))
      DBUG_RETURN(true);
    // TIMOUR TODO: call optimize_cond() for the new having clause
    thd->change_item_tree(&select_lex->having, having_item);
    select_lex->having->top_level_item();
    join_arg->having= select_lex->having;
  }
  SELECT_LEX *global_parameters= unit->global_parameters();
  join_arg->thd->change_item_tree(&global_parameters->limit_params.select_limit,
                                  new (thd->mem_root) Item_int(thd, (int32) 1));
  unit->lim.set_single_row();

  DBUG_RETURN(false);
}


/*
  If this select can potentially be converted by EXISTS->IN conversion, wrap it
  in an Item_in_optimizer object. Final decision whether to do the conversion
  is done at a later phase.
*/

bool Item_exists_subselect::select_prepare_to_be_in()
{
  bool trans_res= FALSE;
  DBUG_ENTER("Item_exists_subselect::select_prepare_to_be_in");
  if (!optimizer &&
      (thd->lex->sql_command == SQLCOM_SELECT ||
       thd->lex->sql_command == SQLCOM_UPDATE_MULTI ||
       thd->lex->sql_command == SQLCOM_DELETE_MULTI) &&
      !unit->first_select()->is_part_of_union() &&
      optimizer_flag(thd, OPTIMIZER_SWITCH_EXISTS_TO_IN) &&
      (is_top_level_item() ||
       (upper_not && upper_not->is_top_level_item())))
  {
    Query_arena *arena, backup;
    bool result;
    arena= thd->activate_stmt_arena_if_needed(&backup);
    result= (!(optimizer= new (thd->mem_root) Item_in_optimizer(thd, new (thd->mem_root) Item_int(thd, 1), this)));
    if (arena)
      thd->restore_active_arena(arena, &backup);
    if (result)
      trans_res= TRUE;
    else
      substitution= optimizer;
  }
  DBUG_RETURN(trans_res);
}

/**
  Check if 'func' is an equality in form "inner_table.column = outer_expr"

  @param func              Expression to check
  @param allow_subselect   If true, the outer_expr part can have a subquery
                           If false, it cannot.
  @param local_field  OUT  Return "inner_table.column" here
  @param outer_expr   OUT  Return outer_expr here

  @return true - 'func' is an Equality.
*/

static bool check_equality_for_exist2in(Item_func *func,
                                        bool allow_subselect,
                                        Item_ident **local_field,
                                        Item **outer_exp)
{
  Item **args;
  if (func->functype() != Item_func::EQ_FUNC)
    return FALSE;
  DBUG_ASSERT(func->argument_count() == 2);
  args= func->arguments();
  if (args[0]->real_type() == Item::FIELD_ITEM &&
      args[0]->all_used_tables() != OUTER_REF_TABLE_BIT &&
      args[1]->all_used_tables() == OUTER_REF_TABLE_BIT &&
      (allow_subselect || !args[1]->with_subquery()))
  {
    /* It is Item_field or Item_direct_view_ref) */
    DBUG_ASSERT(args[0]->type() == Item::FIELD_ITEM ||
                args[0]->type() == Item::REF_ITEM);
    *local_field= (Item_ident *)args[0];
    *outer_exp= args[1];
    return TRUE;
  }
  else if (args[1]->real_type() == Item::FIELD_ITEM &&
           args[1]->all_used_tables() != OUTER_REF_TABLE_BIT &&
           args[0]->all_used_tables() == OUTER_REF_TABLE_BIT &&
           (allow_subselect || !args[0]->with_subquery()))
  {
    /* It is Item_field or Item_direct_view_ref) */
    DBUG_ASSERT(args[1]->type() == Item::FIELD_ITEM ||
                args[1]->type() == Item::REF_ITEM);
    *local_field= (Item_ident *)args[1];
    *outer_exp= args[0];
    return TRUE;
  }

  return FALSE;
}

typedef struct st_eq_field_outer
{
  Item **eq_ref;
  Item_ident *local_field;
  Item *outer_exp;
} EQ_FIELD_OUTER;


/**
  Check if 'conds' is a set of AND-ed outer_expr=inner_table.col equalities

  @detail
    Check if 'conds' has form

    outer1=inner_tbl1.col1 AND ... AND outer2=inner_tbl1.col2 AND remainder_cond

    if there is just one outer_expr=inner_expr pair, then outer_expr can have a
    subselect in it.  If there are many such pairs, then none of outer_expr can
    have a subselect in it. If we allow this, the query will fail with an error:

      This version of MariaDB doesn't yet support 'SUBQUERY in ROW in left
      expression of IN/ALL/ANY'

  @param  conds    Condition to be checked
  @parm   result   Array to collect EQ_FIELD_OUTER elements describing
                   inner-vs-outer equalities the function has found.
  @return
    false - some inner-vs-outer equalities were found
    true  - otherwise.
*/

static bool find_inner_outer_equalities(Item **conds,
                                        Dynamic_array<EQ_FIELD_OUTER> &result)
{
  bool found=  FALSE;
  EQ_FIELD_OUTER element;
  if (is_cond_and(*conds))
  {
    List_iterator<Item> li(*((Item_cond*)*conds)->argument_list());
    Item *item;
    bool allow_subselect= true;
    while ((item= li++))
    {
      if (item->type() == Item::FUNC_ITEM &&
          check_equality_for_exist2in((Item_func *)item,
                                      allow_subselect,
                                      &element.local_field,
                                      &element.outer_exp))
      {
        found= TRUE;
        allow_subselect= false;
        element.eq_ref= li.ref();
        if (result.append(element))
          goto alloc_err;
      }
    }
  }
  else if ((*conds)->type() == Item::FUNC_ITEM &&
           check_equality_for_exist2in((Item_func *)*conds,
                                       true,
                                       &element.local_field,
                                       &element.outer_exp))
  {
    found= TRUE;
    element.eq_ref= conds;
    if (result.append(element))
      goto alloc_err;
  }

  return !found;
alloc_err:
  return TRUE;
}

/**
  Converts EXISTS subquery to IN subquery if it is possible and has sense

  @param opt_arg         Pointer on THD

  @return TRUE in case of error and FALSE otherwise.
*/

bool Item_exists_subselect::exists2in_processor(void *opt_arg)
{
  THD *thd= (THD *)opt_arg;
  SELECT_LEX *first_select=unit->first_select(), *save_select;
  JOIN *join= first_select->join;
  Item **eq_ref= NULL;
  Item_ident *local_field= NULL;
  Item *outer_exp= NULL;
  Item *left_exp= NULL; Item_in_subselect *in_subs;
  Query_arena *arena= NULL, backup;
  int res= FALSE;
  List<Item> outer;
  Dynamic_array<EQ_FIELD_OUTER> eqs(PSI_INSTRUMENT_MEM, 5, 5);
  bool will_be_correlated;
  DBUG_ENTER("Item_exists_subselect::exists2in_processor");

  if (!optimizer ||
      !optimizer_flag(thd, OPTIMIZER_SWITCH_EXISTS_TO_IN) ||
      (!is_top_level_item() && (!upper_not ||
                                !upper_not->is_top_level_item())) ||
      first_select->is_part_of_union() ||
      first_select->group_list.elements ||
      join->having ||
      first_select->with_sum_func ||
      !first_select->leaf_tables.elements||
      !join->conds ||
      with_recursive_reference)
    DBUG_RETURN(FALSE);

  /*
    EXISTS-to-IN coversion and ORDER BY ... LIMIT clause:

    - "[ORDER BY ...] LIMIT n" clause with a non-zero n does not affect
      the result of the EXISTS(...) predicate, and so we can discard
      it during the conversion.
    - "[ORDER BY ...] LIMIT m, n" can turn a non-empty resultset into empty
      one, so it affects tthe EXISTS(...) result and cannot be discarded.

    Disallow exists-to-in conversion if
    (1). three is a LIMIT which is not a basic constant
    (1a)  or is a "LIMIT 0" (see MDEV-19429)
    (2).  there is an OFFSET clause
  */
  if ((first_select->limit_params.select_limit &&                        // (1)
       (!first_select->limit_params.select_limit->basic_const_item() ||  // (1)
        first_select->limit_params.select_limit->val_uint() == 0)) ||    // (1a)
      first_select->limit_params.offset_limit)                           // (2)
  {
    DBUG_RETURN(FALSE);
  }

  /* Disallow the conversion if offset + limit exists */

  DBUG_ASSERT(first_select->group_list.elements == 0 &&
              first_select->having == NULL);

  if (find_inner_outer_equalities(&join->conds, eqs))
    DBUG_RETURN(FALSE);

  DBUG_ASSERT(eqs.elements() != 0);

  save_select= thd->lex->current_select;
  thd->lex->current_select= first_select;

  /* check that the subquery has only dependencies we are going pull out */
  {
    List<Item> unused;
    Collect_deps_prm prm= {&unused,          // parameters
      unit->first_select()->nest_level_base, // nest_level_base
      0,                                     // count
      unit->first_select()->nest_level,      // nest_level
      FALSE                                  // collect
    };
    walk(&Item::collect_outer_ref_processor, TRUE, &prm);
    DBUG_ASSERT(prm.count > 0);
    DBUG_ASSERT(prm.count >= (uint)eqs.elements());
    will_be_correlated= prm.count > (uint)eqs.elements();
    if (upper_not && will_be_correlated)
      goto out;
  }

  if ((uint)eqs.elements() > (first_select->item_list.elements +
                              first_select->select_n_reserved))
    goto out;

  arena= thd->activate_stmt_arena_if_needed(&backup);

  while (first_select->item_list.elements > (uint)eqs.elements())
  {
    first_select->item_list.pop();
    first_select->join->all_fields.elements--;
  }
  {
    List_iterator<Item> it(first_select->item_list);

    for (uint i= 0; i < (uint)eqs.elements(); i++)
    {
      Item *item= it++;
      eq_ref= eqs.at(i).eq_ref;
      local_field= eqs.at(i).local_field;
      outer_exp= eqs.at(i).outer_exp;
      /* Add the field to the SELECT_LIST */
      if (item)
        it.replace(local_field);
      else
      {
        first_select->item_list.push_back(local_field, thd->mem_root);
        first_select->join->all_fields.elements++;
      }
      first_select->ref_pointer_array[i]= (Item *)local_field;

      /* remove the parts from condition */
      if (!upper_not || !local_field->maybe_null())
        *eq_ref= new (thd->mem_root) Item_int(thd, 1);
      else
      {
        *eq_ref= new (thd->mem_root)
          Item_func_isnotnull(thd,
                              new (thd->mem_root)
                              Item_field(thd,
                                         ((Item_field*)(local_field->
                                                        real_item()))->context,
                                         ((Item_field*)(local_field->
                                                        real_item()))->field));
        if((*eq_ref)->fix_fields(thd, (Item **)eq_ref))
        {
          res= TRUE;
          goto out;
        }
      }
      outer_exp->fix_after_pullout(unit->outer_select(), &outer_exp, FALSE);
      outer_exp->update_used_tables();
      outer.push_back(outer_exp, thd->mem_root);
    }
  }

  join->conds->update_used_tables();

  /* make IN SUBQUERY and put outer_exp as left part */
  if (eqs.elements() == 1)
    left_exp= outer_exp;
  else
  {
    if (!(left_exp= new (thd->mem_root) Item_row(thd, outer)))
    {
      res= TRUE;
      goto out;
    }
  }

  /* make EXISTS->IN permanet (see Item_subselect::init()) */
  set_exists_transformed();

  first_select->limit_params.select_limit= NULL;
  first_select->limit_params.explicit_limit= FALSE;
  if (!(in_subs= new (thd->mem_root) Item_in_subselect(thd, left_exp,
                                                         first_select)))
  {
    res= TRUE;
    goto out;
  }
  in_subs->set_exists_transformed();
  optimizer->arguments()[0]= left_exp;
  optimizer->arguments()[1]= in_subs;
  in_subs->optimizer= optimizer;
  DBUG_ASSERT(is_top_level_item() ||
              (upper_not && upper_not->is_top_level_item()));
  in_subs->top_level_item();
  {
    SELECT_LEX *current= thd->lex->current_select;
    optimizer->reset_cache(); // renew cache, and we will not keep it
    thd->lex->current_select= unit->outer_select();
    DBUG_ASSERT(optimizer);
    if (optimizer->fix_left(thd))
    {
      res= TRUE;
      /*
        We should not restore thd->lex->current_select because it will be
        reset on exit from this procedure
      */
      goto out;
    }
    /*
      As far as  Item_ref_in_optimizer do not substitute itself on fix_fields
      we can use same item for all selects.
    */
    in_subs->expr= new (thd->mem_root)
      Item_direct_ref(thd, &first_select->context,
                      (Item**)optimizer->get_cache(),
                      no_matter_name,
                      in_left_expr_name);
    if (in_subs->fix_fields(thd, optimizer->arguments() + 1))
    {
      res= TRUE;
      /*
        We should not restore thd->lex->current_select because it will be
        reset on exit from this procedure
      */
      goto out;
    }
    {
      /* Move dependence list */
      List_iterator_fast<Ref_to_outside> it(upper_refs);
      Ref_to_outside *upper;
      while ((upper= it++))
      {
        uint i;
        for (i= 0; i < (uint)eqs.elements(); i++)
          if (eqs.at(i).outer_exp->
              walk(&Item::find_item_processor, TRUE, upper->item))
            break;
        DBUG_ASSERT(thd->stmt_arena->is_stmt_prepare_or_first_stmt_execute() ||
                    thd->stmt_arena->is_conventional());
        DBUG_ASSERT(thd->stmt_arena->mem_root == thd->mem_root);
        if (i == (uint)eqs.elements() &&
            (in_subs->upper_refs.push_back(
               upper, thd->mem_root)))
          goto out;
      }
    }
    in_subs->update_used_tables();
    /*
      The engine of the subquery is fixed so above fix_fields() is not
      complete and should be fixed
    */
    in_subs->upper_refs= upper_refs;
    upper_refs.empty();
    thd->lex->current_select= current;
  }

  DBUG_ASSERT(unit->item == in_subs);
  DBUG_ASSERT(join == first_select->join);
  /*
    Fix dependency info
  */
  in_subs->is_correlated= will_be_correlated;
  if (!will_be_correlated)
  {
    first_select->uncacheable&= ~UNCACHEABLE_DEPENDENT_GENERATED;
    unit->uncacheable&= ~UNCACHEABLE_DEPENDENT_GENERATED;
  }
  /*
    set possible optimization strategies
  */
  in_subs->emb_on_expr_nest= emb_on_expr_nest;
  res= check_and_do_in_subquery_rewrites(join);
  first_select->join->prepare_stage2();

  first_select->fix_prepare_information(thd, &join->conds, &join->having);

  if (upper_not)
  {
    Item *exp;
    if (eqs.elements() == 1)
    {
      exp= (optimizer->arguments()[0]->maybe_null() ?
            (Item*) new (thd->mem_root)
            Item_cond_and(thd,
                          new (thd->mem_root)
                          Item_func_isnotnull(thd,
                                              new (thd->mem_root)
                                              Item_direct_ref(thd,
                                                              &unit->outer_select()->context,
                                                              optimizer->arguments(),
                                                              no_matter_name,
                                                              exists_outer_expr_name)),
                          optimizer) :
            (Item *)optimizer);
    }
    else
    {
      List<Item> *and_list= new (thd->mem_root) List<Item>;
      if (!and_list)
      {
        res= TRUE;
        goto out;
      }
      for (size_t i= 0; i < eqs.elements(); i++)
      {
        if (optimizer->arguments()[0]->maybe_null())
        {
          and_list->
            push_front(new (thd->mem_root)
                       Item_func_isnotnull(thd,
                                           new (thd->mem_root)
                                           Item_direct_ref(thd,
                                                           &unit->outer_select()->context,
                                                           optimizer->arguments()[0]->addr((int)i),
                                                           no_matter_name,
                                                           exists_outer_expr_name)),
                       thd->mem_root);
        }
      }
      if (and_list->elements > 0)
      {
        and_list->push_front(optimizer, thd->mem_root);
        exp= new (thd->mem_root) Item_cond_and(thd, *and_list);
      }
      else
        exp= optimizer;
    }
    upper_not->arguments()[0]= exp;
    if (exp->fix_fields_if_needed(thd, upper_not->arguments()))
    {
      res= TRUE;
      goto out;
    }
  }

out:
  thd->lex->current_select= save_select;
  if (arena)
    thd->restore_active_arena(arena, &backup);
  DBUG_RETURN(res);
}


/**
  Prepare IN/ALL/ANY/SOME subquery transformation and call the appropriate
  transformation function.

  @param join    JOIN object of transforming subquery

  @notes
  To decide which transformation procedure (scalar or row) applicable here
  we have to call fix_fields() for the left expression to be able to call
  cols() method on it. Also this method makes arena management for
  underlying transformation methods.

  @retval  false  OK
  @retval  true   Error
*/

bool
Item_in_subselect::select_in_like_transformer(JOIN *join)
{
  Query_arena *arena= 0, backup;
  SELECT_LEX *current= thd->lex->current_select;
  THD_WHERE save_where= thd->where;
  bool trans_res= true;
  bool result;

  DBUG_ENTER("Item_in_subselect::select_in_like_transformer");
  DBUG_ASSERT(thd == join->thd);
  thd->where= THD_WHERE::IN_ALL_ANY_SUBQUERY;

  /*
    In some optimisation cases we will not need this Item_in_optimizer
    object, but we can't know it here, but here we need address correct
    reference on left expression.

    note: we won't need Item_in_optimizer when handling degenerate cases
    like "... IN (SELECT 1)"
  */
  arena= thd->activate_stmt_arena_if_needed(&backup);
  if (!optimizer)
  {
    optimizer= new (thd->mem_root) Item_in_optimizer(thd, left_expr_orig, this);
    if ((result= !optimizer))
      goto out;
  }

  thd->lex->current_select= current->return_after_parsing();
  result= optimizer->fix_left(thd);
  thd->lex->current_select= current;

  if (changed)
  {
    trans_res= false;
    goto out;
  }


  if (result)
    goto out;

  /*
    Both transformers call fix_fields() only for Items created inside them,
    and all that items do not make permanent changes in current item arena
    which allow to us call them with changed arena (if we do not know nature
    of Item, we have to call fix_fields() for it only with original arena to
    avoid memory leak)
  */
  if (left_expr->cols() == 1)
    trans_res= single_value_transformer(join);
  else
  {
    /* we do not support row operation for ALL/ANY/SOME */
    if (func != &eq_creator)
    {
      if (arena)
        thd->restore_active_arena(arena, &backup);
      my_error(ER_OPERAND_COLUMNS, MYF(0), 1);
      DBUG_RETURN(true);
    }
    trans_res= row_value_transformer(join);
  }
out:
  if (arena)
    thd->restore_active_arena(arena, &backup);
  thd->where= save_where;
  DBUG_RETURN(trans_res);
}


void Item_in_subselect::print(String *str, enum_query_type query_type)
{
  if (test_strategy(SUBS_IN_TO_EXISTS) &&
      !(query_type & QT_PARSABLE))
    str->append(STRING_WITH_LEN("<exists>"));
  else
  {
    left_expr->print_parenthesised(str, query_type, precedence());
    str->append(STRING_WITH_LEN(" in "));
  }
  Item_subselect::print(str, query_type);
}

bool Item_exists_subselect::fix_fields(THD *thd, Item **ref)
{
  DBUG_ENTER("Item_exists_subselect::fix_fields");
  if (exists_transformed)
    DBUG_RETURN( !( (*ref)= new (thd->mem_root) Item_int(thd, 1)));
  DBUG_RETURN(Item_subselect::fix_fields(thd, ref));
}


bool Item_in_subselect::fix_fields(THD *thd_arg, Item **ref)
{
  uint outer_cols_num;
  List<Item> *inner_cols;
  THD_WHERE save_where= thd_arg->where;
  DBUG_ENTER("Item_in_subselect::fix_fields");

  thd= thd_arg;
  DBUG_ASSERT(unit->thd == thd);

  if (test_strategy(SUBS_SEMI_JOIN))
    DBUG_RETURN( !( (*ref)= new (thd->mem_root) Item_int(thd, 1)) );

  thd->where= THD_WHERE::IN_ALL_ANY_SUBQUERY;
  /*
    Check if the outer and inner IN operands match in those cases when we
    will not perform IN=>EXISTS transformation. Currently this is when we
    use subquery materialization.

    The condition below is true when this method was called recursively from
    inside JOIN::prepare for the JOIN object created by the call chain
    Item_subselect::fix_fields -> subselect_single_select_engine::prepare,
    which creates a JOIN object for the subquery and calls JOIN::prepare for
    the JOIN of the subquery.
    Notice that in some cases, this doesn't happen, and the check_cols()
    test for each Item happens later in
    Item_in_subselect::row_value_in_to_exists_transformer.
    The reason for this mess is that our JOIN::prepare phase works top-down
    instead of bottom-up, so we first do name resoluton and semantic checks
    for the outer selects, then for the inner.
  */
  if (engine &&
      engine->engine_type() == subselect_engine::SINGLE_SELECT_ENGINE &&
      ((subselect_single_select_engine*)engine)->join)
  {
    outer_cols_num= left_expr->cols();

    if (unit->is_unit_op())
      inner_cols= &(unit->types);
    else
      inner_cols= &(unit->first_select()->item_list);
    if (outer_cols_num != inner_cols->elements)
    {
      my_error(ER_OPERAND_COLUMNS, MYF(0), outer_cols_num);
      goto err;
    }
    if (outer_cols_num > 1)
    {
      List_iterator<Item> inner_col_it(*inner_cols);
      Item *inner_col;
      for (uint i= 0; i < outer_cols_num; i++)
      {
        inner_col= inner_col_it++;
        if (inner_col->check_cols(left_expr->element_index(i)->cols()))
          goto err;
      }
    }
  }

  if (left_expr && left_expr->fix_fields_if_needed(thd_arg, &left_expr))
    goto err;
  else
  if (Item_subselect::fix_fields(thd_arg, ref))
    goto err;
  base_flags|= item_base_t::FIXED;
  thd->where= save_where;
  DBUG_RETURN(FALSE);

err:
  thd->where= save_where;
  DBUG_RETURN(TRUE);
}


void Item_in_subselect::fix_after_pullout(st_select_lex *new_parent,
                                          Item **ref, bool merge)
{
  left_expr->fix_after_pullout(new_parent, &left_expr, merge);
  Item_subselect::fix_after_pullout(new_parent, ref, merge);
  used_tables_cache |= left_expr->used_tables();
}

void Item_in_subselect::update_used_tables()
{
  Item_subselect::update_used_tables();
  left_expr->update_used_tables();
  //used_tables_cache |= left_expr->used_tables();
  used_tables_cache= Item_subselect::used_tables() | left_expr->used_tables();
}


/**
  Try to create and initialize an engine to compute a subselect via
  materialization.

  @details
  The method creates a new engine for materialized execution, and initializes
  the engine. The initialization may fail
  - either because it wasn't possible to create the needed temporary table
    and its index,
  - or because of a memory allocation error,

  @returns
    @retval TRUE  memory allocation error occurred
    @retval FALSE an execution method was chosen successfully
*/

bool Item_in_subselect::setup_mat_engine()
{
  subselect_hash_sj_engine       *mat_engine= NULL;
  subselect_single_select_engine *select_engine;

  DBUG_ENTER("Item_in_subselect::setup_mat_engine");
  DBUG_ASSERT(thd);

  /*
    The select_engine (that executes transformed IN=>EXISTS subselects) is
    pre-created at parse time, and is stored in statement memory (preserved
    across PS executions).
  */
  DBUG_ASSERT(engine->engine_type() == subselect_engine::SINGLE_SELECT_ENGINE);
  select_engine= (subselect_single_select_engine*) engine;

  /* Create/initialize execution objects. */
  if (!(mat_engine= new (thd->mem_root)
        subselect_hash_sj_engine(thd, this, select_engine)))
    DBUG_RETURN(TRUE);

  if (mat_engine->prepare(thd) ||
      mat_engine->init(&select_engine->join->fields_list,
                       engine->get_identifier()))
    DBUG_RETURN(TRUE);

  engine= mat_engine;
  DBUG_RETURN(FALSE);
}


/**
  Initialize the cache of the left operand of the IN predicate.

  @note This method has the same purpose as alloc_group_fields(),
  but it takes a different kind of collection of items, and the
  list we push to is dynamically allocated.

  @retval TRUE  if a memory allocation error occurred or the cache is
                not applicable to the current query
  @retval FALSE if success
*/

bool Item_in_subselect::init_left_expr_cache()
{
  JOIN *outer_join;
  DBUG_ASSERT(thd);

  outer_join= unit->outer_select()->join;
  /*
    An IN predicate might be evaluated in a query for which all tables have
    been optimzied away.
  */ 
  if (!outer_join || !outer_join->table_count || !outer_join->tables_list)
    return TRUE;

  if (!(left_expr_cache= new (thd->mem_root) List<Cached_item>))
    return TRUE;

  for (uint i= 0; i < left_expr->cols(); i++)
  {
    Cached_item *cur_item_cache= new_Cached_item(thd,
                                                 left_expr->element_index(i),
                                                 FALSE);
    if (!cur_item_cache || left_expr_cache->push_front(cur_item_cache,
                                                       thd->mem_root))
      return TRUE;
  }
  return FALSE;
}


bool Item_in_subselect::init_cond_guards()
{
  DBUG_ASSERT(thd);
  uint cols_num= left_expr->cols();
  if (!is_top_level_item() && !pushed_cond_guards &&
      (left_expr->maybe_null() || cols_num > 1))
  {
    if (!(pushed_cond_guards= (bool*)thd->alloc(sizeof(bool) * cols_num)))
        return TRUE;
    for (uint i= 0; i < cols_num; i++)
      pushed_cond_guards[i]= TRUE;
  }
  return FALSE;
}

/**
  Initialize the tracker which will be used to provide information for
  the output of EXPLAIN and ANALYZE
*/
void Item_in_subselect::init_subq_materialization_tracker(THD *thd)
{
  if (test_strategy(SUBS_MATERIALIZATION | SUBS_PARTIAL_MATCH_ROWID_MERGE |
                    SUBS_PARTIAL_MATCH_TABLE_SCAN))
  {
    Explain_query *qw= thd->lex->explain;
    DBUG_ASSERT(qw);
    Explain_node *node= qw->get_node(unit->first_select()->select_number);
    if (!node)
      return;
    node->subq_materialization= new(qw->mem_root)
        Explain_subq_materialization(qw->mem_root);
    materialization_tracker= node->subq_materialization->get_tracker();
  }
}


bool
Item_allany_subselect::select_transformer(JOIN *join)
{
  DBUG_ENTER("Item_allany_subselect::select_transformer");
  DBUG_ASSERT((in_strategy & ~(SUBS_MAXMIN_INJECTED | SUBS_MAXMIN_ENGINE |
                               SUBS_IN_TO_EXISTS | SUBS_STRATEGY_CHOSEN)) == 0);
  if (upper_item)
    upper_item->show= 1;
  DBUG_RETURN(select_in_like_transformer(join));
}


void Item_allany_subselect::print(String *str, enum_query_type query_type)
{
  if (test_strategy(SUBS_IN_TO_EXISTS) &&
      !(query_type & QT_PARSABLE))
    str->append(STRING_WITH_LEN("<exists>"));
  else
  {
    left_expr->print(str, query_type);
    str->append(' ');
    const char *name= func->symbol(all);
    str->append(name, strlen(name));
    str->append(all ? " all " : " any ", 5);
  }
  Item_subselect::print(str, query_type);
}


void Item_allany_subselect::no_rows_in_result()
{
  /*
    Subquery predicates outside of the SELECT list must be evaluated in order
    to possibly filter the special result row generated for implicit grouping
    if the subquery is in the HAVING clause.
    If the predicate is constant, we need its actual value in the only result
    row for queries with implicit grouping.
  */
  if (parsing_place != SELECT_LIST || const_item())
    return;
  value= 0;
  null_value= 0;
  was_null= 0;
  make_const();
}


void subselect_engine::set_thd(THD *thd_arg)
{
  thd= thd_arg;
  if (result)
    result->set_thd(thd_arg);
}


subselect_single_select_engine::
subselect_single_select_engine(st_select_lex *select,
			       select_result_interceptor *result_arg,
			       Item_subselect *item_arg)
  :subselect_engine(item_arg, result_arg),
   prepared(0), executed(0),
   select_lex(select), join(0)
{
  select_lex->master_unit()->item= item_arg;
}

int subselect_single_select_engine::get_identifier()
{
  return select_lex->select_number; 
}

void subselect_single_select_engine::force_reexecution()
{ 
  executed= false;
}

void subselect_single_select_engine::cleanup()
{
  DBUG_ENTER("subselect_single_select_engine::cleanup");
  prepared= executed= 0;
  join= 0;
  result->reset_for_next_ps_execution();
  select_lex->uncacheable&= ~UNCACHEABLE_DEPENDENT_INJECTED;
  DBUG_VOID_RETURN;
}


void subselect_union_engine::cleanup()
{
  DBUG_ENTER("subselect_union_engine::cleanup");
  unit->reinit_exec_mechanism();
  result->reset_for_next_ps_execution();
  unit->uncacheable&= ~UNCACHEABLE_DEPENDENT_INJECTED;
  for (SELECT_LEX *sl= unit->first_select(); sl; sl= sl->next_select())
    sl->uncacheable&= ~UNCACHEABLE_DEPENDENT_INJECTED;
  DBUG_VOID_RETURN;
}


bool subselect_union_engine::is_executed() const
{
  return unit->executed;
}

void subselect_union_engine::force_reexecution()
{ 
  unit->executed= false;
}


/*
  Check if last execution of the subquery engine produced any rows

  SYNOPSIS
    subselect_union_engine::no_rows()

  DESCRIPTION
    Check if last execution of the subquery engine produced any rows. The
    return value is undefined if last execution ended in an error.

  RETURN
    TRUE  - Last subselect execution has produced no rows
    FALSE - Otherwise
*/

bool subselect_union_engine::no_rows()
{
  /* Check if we got any rows when reading UNION result from temp. table: */
  if (unit->fake_select_lex)
  {
    JOIN *join= unit->fake_select_lex->join;
    if (join)
      return MY_TEST(!join->send_records);
    return false;
  }
  return MY_TEST(!(((select_union_direct *)(unit->get_union_result()))
                                                            ->send_records));
}


void subselect_uniquesubquery_engine::cleanup()
{
  DBUG_ENTER("subselect_uniquesubquery_engine::cleanup");
  /* 
    Note for mergers: we don't have to, and actually must not de-initialize
    tab->table->file here.
    - We don't have to, because free_tmp_table() will call ha_index_or_rnd_end
    - We must not do it, because tab->table may be a derived table which 
      has been already dropped by close_thread_tables(), while we here are
      called from cleanup_items()
  */
  DBUG_VOID_RETURN;
}


subselect_union_engine::subselect_union_engine(st_select_lex_unit *u,
					       select_result_interceptor *result_arg,
					       Item_subselect *item_arg)
  :subselect_engine(item_arg, result_arg)
{
  unit= u;
  unit->item= item_arg;
}


/**
  Create and prepare the JOIN object that represents the query execution
  plan for the subquery.

  @details
  This method is called from Item_subselect::fix_fields. For prepared
  statements it is called both during the PREPARE and EXECUTE phases in the
  following ways:
  - During PREPARE the optimizer needs some properties
    (join->fields_list.elements) of the JOIN to proceed with preparation of
    the remaining query (namely to complete ::fix_fields for the subselect
    related classes. In the end of PREPARE the JOIN is deleted.
  - When we EXECUTE the query, Item_subselect::fix_fields is called again, and
    the JOIN object is re-created again, prepared and executed. In the end of
    execution it is deleted.
  In all cases the JOIN is created in runtime memory (not in the permanent
  memory root).

  @todo
  Re-check what properties of 'join' are needed during prepare, and see if
  we can avoid creating a JOIN during JOIN::prepare of the outer join.

  @retval 0  if success
  @retval 1  if error
*/

int subselect_single_select_engine::prepare(THD *thd)
{
  if (prepared)
    return 0;
  set_thd(thd);
  if (select_lex->join)
  {
    select_lex->cleanup();
  }
  join= (new (thd->mem_root)
         JOIN(thd, select_lex->item_list,
              select_lex->options | SELECT_NO_UNLOCK, result));
  if (!join || !result)
    return 1; /* Fatal error is set already. */
  prepared= 1;
  SELECT_LEX *save_select= thd->lex->current_select;
  thd->lex->current_select= select_lex;
  if (join->prepare(select_lex->table_list.first,
		    select_lex->where,
		    select_lex->order_list.elements +
		    select_lex->group_list.elements,
		    select_lex->order_list.first,
                    false,
		    select_lex->group_list.first,
		    select_lex->having,
		    NULL, select_lex,
		    select_lex->master_unit()))
    return 1;
  thd->lex->current_select= save_select;
  return 0;
}

int subselect_union_engine::prepare(THD *thd_arg)
{
  set_thd(thd_arg);
  return unit->prepare(unit->derived, result, SELECT_NO_UNLOCK);
}

int subselect_uniquesubquery_engine::prepare(THD *)
{
  /* Should never be called. */
  DBUG_ASSERT(FALSE);
  return 1;
}


/*
  Check if last execution of the subquery engine produced any rows

  SYNOPSIS
    subselect_single_select_engine::no_rows()

  DESCRIPTION
    Check if last execution of the subquery engine produced any rows. The
    return value is undefined if last execution ended in an error.

  RETURN
    TRUE  - Last subselect execution has produced no rows
    FALSE - Otherwise
*/

bool subselect_single_select_engine::no_rows()
{ 
  return !item->assigned();
}


/**
 Makes storage for the output values for the subquery and calcuates
 their data and column types and their nullability.
*/
bool subselect_engine::set_row(List<Item> &item_list, Item_cache **row)
{
  Item *sel_item;
  List_iterator_fast<Item> li(item_list);
  set_handler(&type_handler_varchar);
  for (uint i= 0; (sel_item= li++); i++)
  {
    item->max_length= sel_item->max_length;
    set_handler(sel_item->type_handler());
    item->decimals= sel_item->decimals;
    item->unsigned_flag= sel_item->unsigned_flag;
    maybe_null= sel_item->maybe_null();
    if (!(row[i]= sel_item->get_cache(thd)))
      return TRUE;
    row[i]->setup(thd, sel_item);
 //psergey-backport-timours:   row[i]->store(sel_item);
  }
  if (item_list.elements > 1)
    set_handler(&type_handler_row);
  return FALSE;
}

bool subselect_single_select_engine::fix_length_and_dec(Item_cache **row)
{
  DBUG_ASSERT(row || select_lex->item_list.elements==1);
  if (set_row(select_lex->item_list, row))
    return TRUE;
  item->collation.set(row[0]->collation);
  if (cols() != 1)
    maybe_null= 0;
  return FALSE;
}

bool subselect_union_engine::fix_length_and_dec(Item_cache **row)
{
  DBUG_ASSERT(row || unit->first_select()->item_list.elements==1);

  if (unit->first_select()->item_list.elements == 1)
  {
    if (set_row(unit->item_list, row))
      return TRUE;
    item->collation.set(row[0]->collation);
  }
  else
  {
    bool maybe_null_saved= maybe_null;
    if (set_row(unit->item_list, row))
      return TRUE;
    maybe_null= maybe_null_saved;
  }
  return FALSE;
}

bool subselect_uniquesubquery_engine::fix_length_and_dec(Item_cache **row)
{
  //this never should be called
  DBUG_ASSERT(0);
  return FALSE;
}

int  read_first_record_seq(JOIN_TAB *tab);
int rr_sequential(READ_RECORD *info);
int join_read_always_key_or_null(JOIN_TAB *tab);
int join_read_next_same_or_null(READ_RECORD *info);

int subselect_single_select_engine::exec()
{
  THD_WHERE save_where= thd->where;
  SELECT_LEX *save_select= thd->lex->current_select;
  thd->lex->current_select= select_lex;
  DBUG_ENTER("subselect_single_select_engine::exec");

  if (join->optimization_state == JOIN::NOT_OPTIMIZED)
  {
    SELECT_LEX_UNIT *unit= select_lex->master_unit();

    unit->set_limit(unit->global_parameters());
    if (join->optimize())
    {
      thd->where= save_where;
      executed= 1;
      thd->lex->current_select= save_select;
      DBUG_RETURN(join->error ? join->error : 1);
    }
    if (!select_lex->uncacheable && thd->lex->describe && 
        !(join->select_options & SELECT_DESCRIBE))
    {
      item->update_used_tables();
      if (item->const_item())
      {
        /*
          It's necessary to keep original JOIN table because
          create_sort_index() function may overwrite original
          JOIN_TAB::type and wrong optimization method can be
          selected on re-execution.
        */
        select_lex->uncacheable|= UNCACHEABLE_EXPLAIN;
        select_lex->master_unit()->uncacheable|= UNCACHEABLE_EXPLAIN;
      }
    }
    if (item->engine_changed(this))
    {
      thd->lex->current_select= save_select;
      DBUG_RETURN(1);
    }
  }
  if (select_lex->uncacheable &&
      select_lex->uncacheable != UNCACHEABLE_EXPLAIN
      && executed)
  {
    if (join->reinit())
    {
      thd->where= save_where;
      thd->lex->current_select= save_select;
      DBUG_RETURN(1);
    }
    item->reset();
    item->assigned((executed= 0));
  }
  if (!executed)
  {
    item->reset_value_registration();
    JOIN_TAB *changed_tabs[MAX_TABLES];
    JOIN_TAB **last_changed_tab= changed_tabs;
    if (item->have_guarded_conds())
    {
      /*
        For at least one of the pushed predicates the following is true:
        We should not apply optimizations based on the condition that was
        pushed down into the subquery. Those optimizations are ref[_or_null]
        accesses. Change them to be full table scans.
      */
      JOIN_TAB *tab;
      for (tab= first_linear_tab(join, WITH_BUSH_ROOTS, WITHOUT_CONST_TABLES);
           tab; tab= next_linear_tab(join, tab, WITH_BUSH_ROOTS))
      {
        if (tab && tab->keyuse)
        {
          for (uint i= 0; i < tab->ref.key_parts; i++)
          {
            bool *cond_guard= tab->ref.cond_guards[i];
            if (cond_guard && !*cond_guard)
            {
              /* Change the access method to full table scan */
              tab->save_read_first_record= tab->read_first_record;
              tab->save_read_record= tab->read_record.read_record_func;
              tab->read_record.read_record_func= rr_sequential;
              tab->read_first_record= read_first_record_seq;
              if (tab->rowid_filter)
                tab->table->file->disable_pushed_rowid_filter();
              tab->read_record.thd= join->thd;
              tab->read_record.ref_length= tab->table->file->ref_length;
              tab->read_record.unlock_row= rr_unlock_row;
              *(last_changed_tab++)= tab;
              break;
            }
          }
        }
      }
    }
    
    join->exec();

    /* Enable the optimizations back */
    for (JOIN_TAB **ptab= changed_tabs; ptab != last_changed_tab; ptab++)
    {
      JOIN_TAB *tab= *ptab;
      tab->read_record.ref_length= 0;
      tab->read_first_record= tab->save_read_first_record;
      tab->read_record.read_record_func= tab->save_read_record;
      if (tab->rowid_filter)
        tab->table->file->enable_pushed_rowid_filter();
    }
    executed= 1;
    if (!(uncacheable() & ~UNCACHEABLE_EXPLAIN) &&
        !item->with_recursive_reference)
      item->make_const();
    thd->where= save_where;
    thd->lex->current_select= save_select;
    DBUG_RETURN(join->error || thd->is_fatal_error || thd->is_error());
  }
  thd->where= save_where;
  thd->lex->current_select= save_select;
  DBUG_RETURN(0);
}

int subselect_union_engine::exec()
{
  THD_WHERE save_where= thd->where;
  int res= unit->exec();
  thd->where= save_where;
  return res;
}


/*
  Search for at least one row satisfying select condition
 
  SYNOPSIS
    subselect_uniquesubquery_engine::scan_table()

  DESCRIPTION
    Scan the table using sequential access until we find at least one row
    satisfying select condition.
    
    The caller must set this->empty_result_set=FALSE before calling this
    function. This function will set it to TRUE if it finds a matching row.

  RETURN
    FALSE - OK
    TRUE  - Error
*/

int subselect_uniquesubquery_engine::scan_table()
{
  int error;
  TABLE *table= tab->table;
  DBUG_ENTER("subselect_uniquesubquery_engine::scan_table");

  if ((table->file->inited &&
       (error= table->file->ha_index_end())) ||
      (error= table->file->ha_rnd_init(1)))
  {
    (void) report_error(table, error);
    DBUG_RETURN(true);
  }

  table->file->extra_opt(HA_EXTRA_CACHE,
                         get_thd()->variables.read_buff_size);
  table->null_row= 0;
  for (;;)
  {
    error=table->file->ha_rnd_next(table->record[0]);
    if (unlikely(error))
    {
      if (error == HA_ERR_END_OF_FILE)
      {
        error= 0;
        break;
      }
      else
      {
        error= report_error(table, error);
        break;
      }
    }

    if (!cond || cond->val_bool())
    {
      empty_result_set= FALSE;
      break;
    }
  }

  table->file->ha_rnd_end();
  DBUG_RETURN(error != 0);
}


/**
  Copy ref key for index access into the only subquery table.

  @details
    Copy ref key and check for conversion problems.
    If there is an error converting the left IN operand to the column type of
    the right IN operand count it as no match. In this case IN has the value of
    FALSE. We mark the subquery table cursor as having no more rows (to ensure
    that the processing that follows will not find a match) and return FALSE,
    so IN is not treated as returning NULL.

  @returns
    @retval FALSE The outer ref was copied into an index lookup key.
    @retval TRUE  The outer ref cannot possibly match any row, IN is FALSE.
*/

bool subselect_uniquesubquery_engine::copy_ref_key(bool skip_constants)
{
  DBUG_ENTER("subselect_uniquesubquery_engine::copy_ref_key");

  for (store_key **copy= tab->ref.key_copy ; *copy ; copy++)
  {
    enum store_key::store_key_result store_res;
    if (skip_constants && (*copy)->store_key_is_const())
      continue;
    store_res= (*copy)->copy(thd);
    tab->ref.key_err= store_res;

    if (store_res == store_key::STORE_KEY_FATAL)
    {
      /*
       Error converting the left IN operand to the column type of the right
       IN operand. 
      */
      DBUG_RETURN(true);
    }
  }
  DBUG_RETURN(false);
}


/**
  Execute subselect via unique index lookup

  @details
    Find rows corresponding to the ref key using index access.
    If some part of the lookup key is NULL, then we're evaluating
      NULL IN (SELECT ... )
    This is a special case, we don't need to search for NULL in the table,
    instead, the result value is 
      - NULL  if select produces empty row set
      - FALSE otherwise.

    In some cases (IN subselect is a top level item, i.e.
    is_top_level_item() == TRUE, the caller doesn't distinguish between NULL and
    FALSE result and we just return FALSE.
    Otherwise we make a full table scan to see if there is at least one 
    matching row.
    
    The result of this function (info about whether a row was found) is
    stored in this->empty_result_set.
    
  @returns
    @retval 0  OK
    @retval 1  notify caller to call Item_subselect::reset(),
               in most cases reset() sets the result to NULL
*/

int subselect_uniquesubquery_engine::exec()
{
  DBUG_ENTER("subselect_uniquesubquery_engine::exec");
  int error;
  TABLE *table= tab->table;
  empty_result_set= TRUE;
  table->status= 0;
  Item_in_subselect *in_subs= item->get_IN_subquery();
  Subq_materialization_tracker *tracker= in_subs->get_materialization_tracker();
  DBUG_ASSERT(in_subs);
  DBUG_ASSERT(thd);

  if (!tab->preread_init_done && tab->preread_init())
    DBUG_RETURN(1);
  if (tracker)
    tracker->increment_loops_count();
  if (in_subs->left_expr_has_null())
  {
    /*
      The case when all values in left_expr are NULL is handled by
      Item_in_optimizer::val_int().
    */
    if (in_subs->is_top_level_item())
      DBUG_RETURN(1); /* notify caller to call reset() and set NULL value. */
    else
      DBUG_RETURN(scan_table());
  }

  if (copy_ref_key(true))
  {
    /* We know that there will be no rows even if we scan. */
    in_subs->value= 0;
    DBUG_RETURN(0);
  }

  if (!table->file->inited &&
      (error= table->file->ha_index_init(tab->ref.key, 0)))
  {
    (void) report_error(table, error);
    DBUG_RETURN(true);
  }

  error= table->file->ha_index_read_map(table->record[0],
                                        tab->ref.key_buff,
                                        make_prev_keypart_map(tab->
                                                              ref.key_parts),
                                        HA_READ_KEY_EXACT);
  if (unlikely(error &&
               error != HA_ERR_KEY_NOT_FOUND && error != HA_ERR_END_OF_FILE))
    error= report_error(table, error);
  else
  {
    error= 0;
    table->null_row= 0;
    if (!table->status && (!cond || cond->val_int()))
    {
      in_subs->value= 1;
      empty_result_set= FALSE;
    }
    else
      in_subs->value= 0;
  }

  DBUG_RETURN(error != 0);
}


/*
  TIMOUR: write comment
*/

int subselect_uniquesubquery_engine::index_lookup()
{
  DBUG_ENTER("subselect_uniquesubquery_engine::index_lookup");
  int error;
  TABLE *table= tab->table;
 
  if (!table->file->inited)
    table->file->ha_index_init(tab->ref.key, 0);
  error= table->file->ha_index_read_map(table->record[0],
                                        tab->ref.key_buff,
                                        make_prev_keypart_map(tab->
                                                              ref.key_parts),
                                        HA_READ_KEY_EXACT);
  DBUG_PRINT("info", ("lookup result: %i", error));

  if (unlikely(error && error != HA_ERR_KEY_NOT_FOUND &&
               error != HA_ERR_END_OF_FILE))
  {
    /*
      TIMOUR: I don't understand at all when do we need to call report_error.
      In most places where we access an index, we don't do this. Why here?
    */
    error= report_error(table, error);
    DBUG_RETURN(error);
  }

  table->null_row= 0;
  if (!error && (!cond || cond->val_int()))
    item->get_IN_subquery()->value= 1;
  else
    item->get_IN_subquery()->value= 0;

  DBUG_RETURN(0);
}



subselect_uniquesubquery_engine::~subselect_uniquesubquery_engine()
{
  /* Tell handler we don't need the index anymore */
  //psergey-merge-todo: the following was gone in 6.0:
 //psergey-merge: don't need this after all: tab->table->file->ha_index_end();
}


/**
  Execute subselect via unique index lookup

  @details
    The engine is used to resolve subqueries in form

      oe IN (SELECT key FROM tbl WHERE subq_where) 

    The value of the predicate is calculated as follows: 
    1. If oe IS NULL, this is a special case, do a full table scan on
       table tbl and search for row that satisfies subq_where. If such 
       row is found, return NULL, otherwise return FALSE.
    2. Make an index lookup via key=oe, search for a row that satisfies
       subq_where. If found, return TRUE.
    3. If check_null==TRUE, make another lookup via key=NULL, search for a 
       row that satisfies subq_where. If found, return NULL, otherwise
       return FALSE.

  @todo
    The step #1 can be optimized further when the index has several key
    parts. Consider a subquery:
    
      (oe1, oe2) IN (SELECT keypart1, keypart2 FROM tbl WHERE subq_where)

    and suppose we need to evaluate it for {oe1, oe2}=={const1, NULL}.
    Current code will do a full table scan and obtain correct result. There
    is a better option: instead of evaluating

      SELECT keypart1, keypart2 FROM tbl WHERE subq_where            (1)

    and checking if it has produced any matching rows, evaluate
    
      SELECT keypart2 FROM tbl WHERE subq_where AND keypart1=const1  (2)

    If this query produces a row, the result is NULL (as we're evaluating 
    "(const1, NULL) IN { (const1, X), ... }", which has a value of UNKNOWN,
    i.e. NULL).  If the query produces no rows, the result is FALSE.

    We currently evaluate (1) by doing a full table scan. (2) can be
    evaluated by doing a "ref" scan on "keypart1=const1", which can be much
    cheaper. We can use index statistics to quickly check whether "ref" scan
    will be cheaper than full table scan.

  @returns
    @retval 0  OK
    @retval 1  notify caller to call Item_subselect::reset(),
               in most cases reset() sets the result to NULL
*/

int subselect_indexsubquery_engine::exec()
{
  DBUG_ENTER("subselect_indexsubquery_engine");
  int error;
  bool null_finding= 0;
  TABLE *table= tab->table;
  Item_in_subselect *in_subs= item->get_IN_subquery();
  DBUG_ASSERT(thd);

  in_subs->value= 0;
  empty_result_set= TRUE;
  table->status= 0;

  if (check_null)
  {
    /* We need to check for NULL if there wasn't a matching value */
    *tab->ref.null_ref_key= 0;			// Search first for not null
    in_subs->was_null= 0;
  }

  if (!tab->preread_init_done && tab->preread_init())
    DBUG_RETURN(1);

  if (in_subs->left_expr_has_null())
  {
    /*
      The case when all values in left_expr are NULL is handled by
      Item_in_optimizer::val_int().
    */
    if (in_subs->is_top_level_item())
      DBUG_RETURN(1); /* notify caller to call reset() and set NULL value. */
    else
      DBUG_RETURN(scan_table());
  }

  if (copy_ref_key(true))
  {
    /* We know that there will be no rows even if we scan. */
    in_subs->value= 0;
    DBUG_RETURN(0);
  }

  if (!table->file->inited &&
      (error= table->file->ha_index_init(tab->ref.key, 1)))
  {
    (void) report_error(table, error);
    DBUG_RETURN(true);
  }

  error= table->file->ha_index_read_map(table->record[0],
                                        tab->ref.key_buff,
                                        make_prev_keypart_map(tab->
                                                              ref.key_parts),
                                        HA_READ_KEY_EXACT);
  if (unlikely(error &&
               error != HA_ERR_KEY_NOT_FOUND && error != HA_ERR_END_OF_FILE))
    error= report_error(table, error);
  else
  {
    for (;;)
    {
      error= 0;
      table->null_row= 0;
      if (!table->status)
      {
        if ((!cond || cond->val_bool()) && (!having || having->val_bool()))
        {
          empty_result_set= FALSE;
          if (null_finding)
            in_subs->was_null= 1;
          else
            in_subs->value= 1;
          break;
        }
        error= table->file->ha_index_next_same(table->record[0],
                                               tab->ref.key_buff,
                                               tab->ref.key_length);
        if (unlikely(error && error != HA_ERR_END_OF_FILE))
        {
          error= report_error(table, error);
          break;
        }
      }
      else
      {
        if (!check_null || null_finding)
          break;			/* We don't need to check nulls */
        *tab->ref.null_ref_key= 1;
        null_finding= 1;
        /* Check if there exists a row with a null value in the index */
        if (unlikely((error= (safe_index_read(tab) == 1))))
          break;
      }
    }
  }
  DBUG_RETURN(error != 0);
}


uint subselect_single_select_engine::cols() const
{
  //psergey-sj-backport: the following assert was gone in 6.0:
  //DBUG_ASSERT(select_lex->join != 0); // should be called after fix_fields()
  //return select_lex->join->fields_list.elements;
  return select_lex->item_list.elements;
}


uint subselect_union_engine::cols() const
{
  DBUG_ASSERT(unit->is_prepared());  // should be called after fix_fields()
  return unit->types.elements;
}


uint8 subselect_single_select_engine::uncacheable()
{
  return select_lex->uncacheable;
}


uint8 subselect_union_engine::uncacheable()
{
  return unit->uncacheable;
}


void subselect_single_select_engine::exclude()
{
  select_lex->master_unit()->exclude_level();
}

void subselect_union_engine::exclude()
{
  unit->exclude_level();
}


void subselect_uniquesubquery_engine::exclude()
{
  //this never should be called
  DBUG_ASSERT(0);
}


table_map subselect_engine::calc_const_tables(List<TABLE_LIST> &list)
{
  table_map map= 0;
  List_iterator<TABLE_LIST> ti(list);
  TABLE_LIST *table;
  //for (; table; table= table->next_leaf)
  while ((table= ti++))
  {
    TABLE *tbl= table->table;
    if (tbl && tbl->const_table)
      map|= tbl->map;
  }
  return map;
}


table_map subselect_single_select_engine::upper_select_const_tables()
{
  return calc_const_tables(select_lex->outer_select()->leaf_tables);
}


table_map subselect_union_engine::upper_select_const_tables()
{
  return calc_const_tables(unit->outer_select()->leaf_tables);
}


void subselect_single_select_engine::print(String *str,
                                           enum_query_type query_type)
{
  With_clause* with_clause= select_lex->get_with_clause();
  THD *thd= get_thd();
  if (with_clause)
    with_clause->print(thd, str, query_type);
  select_lex->print(thd, str, query_type);
}


void subselect_union_engine::print(String *str, enum_query_type query_type)
{
  unit->print(str, query_type);
}


void subselect_uniquesubquery_engine::print(String *str,
                                            enum_query_type query_type)
{
  TABLE *table= tab->tab_list ? tab->tab_list->table : tab->table;
  str->append(STRING_WITH_LEN("<primary_index_lookup>("));
  tab->ref.items[0]->print(str, query_type);
  str->append(STRING_WITH_LEN(" in "));
  if (table->s->table_category == TABLE_CATEGORY_TEMPORARY)
  {
    /*
      Temporary tables' names change across runs, so they can't be used for
      EXPLAIN EXTENDED.
    */
    str->append(STRING_WITH_LEN("<temporary table>"));
  }
  else
    str->append(&table->s->table_name);
  KEY *key_info= table->key_info+ tab->ref.key;
  str->append(STRING_WITH_LEN(" on "));
  str->append(&key_info->name);
  if (cond)
  {
    str->append(STRING_WITH_LEN(" where "));
    cond->print(str, query_type);
  }
  str->append(')');
}

/*
TODO:
The above ::print method should be changed as below. Do it after
all other tests pass.

void subselect_uniquesubquery_engine::print(String *str)
{
  TABLE *table= tab->tab_list ? tab->tab_list->table : tab->table;
  KEY *key_info= table->key_info + tab->ref.key;
  str->append(STRING_WITH_LEN("<primary_index_lookup>("));
  for (uint i= 0; i < key_info->user_defined_key_parts; i++)
    tab->ref.items[i]->print(str);
  str->append(STRING_WITH_LEN(" in "));
  str->append(&table->s->table_name);
  str->append(STRING_WITH_LEN(" on "));
  str->append(&key_info->name);
  if (cond)
  {
    str->append(STRING_WITH_LEN(" where "));
    cond->print(str);
  }
  str->append(')');
}
*/

void subselect_indexsubquery_engine::print(String *str,
                                           enum_query_type query_type)
{
  TABLE *table= tab->tab_list ? tab->tab_list->table : tab->table;
  str->append(STRING_WITH_LEN("<index_lookup>("));
  tab->ref.items[0]->print(str, query_type);
  str->append(STRING_WITH_LEN(" in "));
  str->append(&table->s->table_name);
  KEY *key_info= table->key_info+ tab->ref.key;
  str->append(STRING_WITH_LEN(" on "));
  str->append(&key_info->name);
  if (check_null)
    str->append(STRING_WITH_LEN(" checking NULL"));
  if (cond)
  {
    str->append(STRING_WITH_LEN(" where "));
    cond->print(str, query_type);
  }
  if (having)
  {
    str->append(STRING_WITH_LEN(" having "));
    having->print(str, query_type);
  }
  str->append(')');
}

/**
  change select_result object of engine.

  @param si		new subselect Item
  @param res		new select_result object
  @param temp           temporary assignment

  @retval
    FALSE OK
  @retval
    TRUE  error
*/

bool
subselect_single_select_engine::change_result(Item_subselect *si,
                                              select_result_interceptor *res,
                                              bool temp)
{
  DBUG_ENTER("subselect_single_select_engine::change_result");
  item= si;
  if (temp)
  {
    /*
      Here we reuse change_item_tree to roll back assignment.  It has
      nothing special about Item* pointer so it is safe conversion. We do
      not change the interface to be compatible with MySQL.
    */
    thd->change_item_tree((Item**) &result, (Item*)res);
  }
  else
    result= res;

  /*
    We can't use 'result' below as gcc 4.2.4's alias optimization
    assumes that result was not changed by thd->change_item_tree().
    I tried to find a solution to make gcc happy, but could not find anything
    that would not require a lot of extra code that would be harder to manage
    than the current code.
  */
  DBUG_RETURN(select_lex->join->change_result(res, NULL));
}


/**
  change select_result object of engine.

  @param si		new subselect Item
  @param res		new select_result object

  @retval
    FALSE OK
  @retval
    TRUE  error
*/

bool subselect_union_engine::change_result(Item_subselect *si,
                                           select_result_interceptor *res,
                                           bool temp)
{
  item= si;
  int rc= unit->change_result(res, result);
  if (temp)
    thd->change_item_tree((Item**) &result, (Item*)res);
  else
    result= res;
  return rc;
}


/**
  change select_result emulation, never should be called.

  @param si		new subselect Item
  @param res		new select_result object

  @retval
    FALSE OK
  @retval
    TRUE  error
*/

bool
subselect_uniquesubquery_engine::change_result(Item_subselect *si,
                                               select_result_interceptor *res,
                                               bool temp
                                               __attribute__((unused)))
{
  DBUG_ASSERT(0);
  return TRUE;
}


/**
  Report about presence of tables in subquery.

  @retval
    TRUE  there are not tables used in subquery
  @retval
    FALSE there are some tables in subquery
*/
bool subselect_single_select_engine::no_tables() const
{
  return(select_lex->table_list.elements == 0);
}


/*
  Check statically whether the subquery can return NULL

  SINOPSYS
    subselect_single_select_engine::may_be_null()

  RETURN
    FALSE  can guarantee that the subquery never return NULL
    TRUE   otherwise
*/
bool subselect_single_select_engine::may_be_null()
{
  return ((no_tables() && !join->conds && !join->having) ? maybe_null : 1);
}


/**
  Report about presence of tables in subquery.

  @retval
    TRUE  there are not tables used in subquery
  @retval
    FALSE there are some tables in subquery
*/
bool subselect_union_engine::no_tables() const
{
  for (SELECT_LEX *sl= unit->first_select(); sl; sl= sl->next_select())
  {
    if (sl->table_list.elements)
      return FALSE;
  }
  return TRUE;
}


/**
  Report about presence of tables in subquery.

  @retval
    TRUE  there are not tables used in subquery
  @retval
    FALSE there are some tables in subquery
*/

bool subselect_uniquesubquery_engine::no_tables() const
{
  /* returning value is correct, but this method should never be called */
  DBUG_ASSERT(FALSE);
  return 0;
}


/******************************************************************************
  WL#1110 - Implementation of class subselect_hash_sj_engine
******************************************************************************/


/**
  Check if an IN predicate should be executed via partial matching using
  only schema information.

  @details
  This test essentially has three results:
  - partial matching is applicable, but cannot be executed due to a
    limitation in the total number of indexes, as a result we can't
    use subquery materialization at all.
  - partial matching is either applicable or not, and this can be
    determined by looking at 'this->max_keys'.
  If max_keys > 1, then we need partial matching because there are
  more indexes than just the one we use during materialization to
  remove duplicates.

  @note
  TIMOUR: The schema-based analysis for partial matching can be done once for
  prepared statement and remembered. It is done here to remove the need to
  save/restore all related variables between each re-execution, thus making
  the code simpler.

  @retval PARTIAL_MATCH  if a partial match should be used
  @retval COMPLETE_MATCH if a complete match (index lookup) should be used
*/

subselect_hash_sj_engine::exec_strategy
subselect_hash_sj_engine::get_strategy_using_schema()
{
  Item_in_subselect *item_in= item->get_IN_subquery();

  if (item_in->is_top_level_item())
    return COMPLETE_MATCH;
  else
  {
    List_iterator<Item> inner_col_it(*item_in->unit->get_column_types(false));
    Item *outer_col, *inner_col;

    for (uint i= 0; i < item_in->left_expr->cols(); i++)
    {
      outer_col= item_in->left_expr->element_index(i);
      inner_col= inner_col_it++;

      if (!inner_col->maybe_null() && !outer_col->maybe_null())
        bitmap_set_bit(&non_null_key_parts, i);
      else
      {
        bitmap_set_bit(&partial_match_key_parts, i);
        ++count_partial_match_columns;
      }
    }
  }

  /* If no column contains NULLs use regular hash index lookups. */
  if (count_partial_match_columns)
    return PARTIAL_MATCH;
  return COMPLETE_MATCH;
}


/**
  Test whether an IN predicate must be computed via partial matching
  based on the NULL statistics for each column of a materialized subquery.

  @details The procedure analyzes column NULL statistics, updates the
  matching type of columns that cannot be NULL or that contain only NULLs.
  Based on this, the procedure determines the final execution strategy for
  the [NOT] IN predicate.

  @retval PARTIAL_MATCH  if a partial match should be used
  @retval COMPLETE_MATCH if a complete match (index lookup) should be used
*/

subselect_hash_sj_engine::exec_strategy
subselect_hash_sj_engine::get_strategy_using_data()
{
  Item_in_subselect *item_in= item->get_IN_subquery();
  select_materialize_with_stats *result_sink=
    (select_materialize_with_stats *) result;
  Item *outer_col;

  /*
    If we already determined that a complete match is enough based on schema
    information, nothing can be better.
  */
  if (strategy == COMPLETE_MATCH)
    return COMPLETE_MATCH;

  for (uint i= 0; i < item_in->left_expr->cols(); i++)
  {
    if (!bitmap_is_set(&partial_match_key_parts, i))
      continue;
    outer_col= item_in->left_expr->element_index(i);
    /*
      If column 'i' doesn't contain NULLs, and the corresponding outer reference
      cannot have a NULL value, then 'i' is a non-nullable column.
    */
    if (result_sink->get_null_count_of_col(i) == 0 && !outer_col->maybe_null())
    {
      bitmap_clear_bit(&partial_match_key_parts, i);
      bitmap_set_bit(&non_null_key_parts, i);
      --count_partial_match_columns;
    }
    if (result_sink->get_null_count_of_col(i) == tmp_table->file->stats.records)
      ++count_null_only_columns;
    if (result_sink->get_null_count_of_col(i))
      ++count_columns_with_nulls;
  }

  /* If no column contains NULLs use regular hash index lookups. */
  if (!count_partial_match_columns)
    return COMPLETE_MATCH;
  return PARTIAL_MATCH;
}


void
subselect_hash_sj_engine::choose_partial_match_strategy(
  uint field_count, bool has_non_null_key, bool has_covering_null_row,
  MY_BITMAP *partial_match_key_parts_arg)
{
  ulonglong pm_buff_size;

  DBUG_ASSERT(strategy == PARTIAL_MATCH);
  if (field_count == 1)
  {
    strategy= SINGLE_COLUMN_MATCH;
    return;
  }

  /*
    Choose according to global optimizer switch. If only one of the switches is
    'ON', then the remaining strategy is the only possible one. The only cases
    when this will be overridden is when the total size of all buffers for the
    merge strategy is bigger than the 'rowid_merge_buff_size' system variable,
    or if there isn't enough physical memory to allocate the buffers.
  */
  if (!optimizer_flag(thd, OPTIMIZER_SWITCH_PARTIAL_MATCH_ROWID_MERGE) &&
       optimizer_flag(thd, OPTIMIZER_SWITCH_PARTIAL_MATCH_TABLE_SCAN))
    strategy= PARTIAL_MATCH_SCAN;
  else if
     ( optimizer_flag(thd, OPTIMIZER_SWITCH_PARTIAL_MATCH_ROWID_MERGE) &&
      !optimizer_flag(thd, OPTIMIZER_SWITCH_PARTIAL_MATCH_TABLE_SCAN))
    strategy= PARTIAL_MATCH_MERGE;

  /*
    If both switches are ON, or both are OFF, we interpret that as "let the
    optimizer decide". Perform a cost based choice between the two partial
    matching strategies.
  */
  /*
    TIMOUR: the above interpretation of the switch values could be changed to:
    - if both are ON - let the optimizer decide,
    - if both are OFF - do not use partial matching, therefore do not use
      materialization in non-top-level predicates.
    The problem with this is that we know for sure if we need partial matching
    only after the subquery is materialized, and this is too late to revert to
    the IN=>EXISTS strategy.
  */
  if (strategy == PARTIAL_MATCH)
  {
    /*
      TIMOUR: Currently we use a super simplistic measure. This will be
      addressed in a separate task.
    */
    if (tmp_table->file->stats.records < 100)
      strategy= PARTIAL_MATCH_SCAN;
    else
      strategy= PARTIAL_MATCH_MERGE;
  }

  /* Check if there is enough memory for the rowid merge strategy. */
  if (strategy == PARTIAL_MATCH_MERGE)
  {
    pm_buff_size= rowid_merge_buff_size(has_non_null_key,
                                        has_covering_null_row,
                                        partial_match_key_parts_arg);
    if (pm_buff_size > thd->variables.rowid_merge_buff_size)
      strategy= PARTIAL_MATCH_SCAN;
    else
      item->get_IN_subquery()->get_materialization_tracker()->
          report_partial_match_buffer_size(pm_buff_size);
  }
}


/*
  Compute the memory size of all buffers proportional to the number of rows
  in tmp_table.

  @details
  If the result is bigger than thd->variables.rowid_merge_buff_size, partial
  matching via merging is not applicable.
*/

ulonglong subselect_hash_sj_engine::rowid_merge_buff_size(
  bool has_non_null_key, bool has_covering_null_row,
  MY_BITMAP *partial_match_key_parts)
{
  /* Total size of all buffers used by partial matching. */
  ulonglong buff_size;
  ha_rows row_count= tmp_table->file->stats.records;
  uint rowid_length= tmp_table->file->ref_length;
  select_materialize_with_stats *result_sink=
    (select_materialize_with_stats *) result;
  ha_rows max_null_row;

  /* Size of the subselect_rowid_merge_engine::row_num_to_rowid buffer. */
  buff_size= row_count * rowid_length * sizeof(uchar);

  if (has_non_null_key)
  {
    /* Add the size of Ordered_key::key_buff of the only non-NULL key. */
    buff_size+= row_count * sizeof(rownum_t);
  }

  if (!has_covering_null_row)
  {
    for (uint i= 0; i < partial_match_key_parts->n_bits; i++)
    {
      if (!bitmap_is_set(partial_match_key_parts, i) ||
          result_sink->get_null_count_of_col(i) == row_count)
        continue; /* In these cases we wouldn't construct Ordered keys. */

      /* Add the size of Ordered_key::key_buff */
      buff_size+= (row_count - result_sink->get_null_count_of_col(i)) *
                         sizeof(rownum_t);
      /* Add the size of Ordered_key::null_key */
      max_null_row= result_sink->get_max_null_of_col(i);
      if (max_null_row >= UINT_MAX)
      {
        /*
          There can be at most UINT_MAX bits in a MY_BITMAP that is used to
          store NULLs in an Ordered_key. Return a number of bytes bigger than
          the maximum allowed memory buffer for partial matching to disable
          the rowid merge strategy.
        */
        return ULONGLONG_MAX;
      }
      buff_size+= bitmap_buffer_size(max_null_row);
    }
  }

  return buff_size;
}


/*
  Initialize a MY_BITMAP with a buffer allocated on the current
  memory root.
  TIMOUR: move to bitmap C file?
*/

static my_bool
my_bitmap_init_memroot(MY_BITMAP *map, uint n_bits, MEM_ROOT *mem_root)
{
  my_bitmap_map *bitmap_buf;

  if (!(bitmap_buf= (my_bitmap_map*) alloc_root(mem_root,
                                                bitmap_buffer_size(n_bits))) ||
      my_bitmap_init(map, bitmap_buf, n_bits))
    return TRUE;
  bitmap_clear_all(map);
  return FALSE;
}


/**
  Create all structures needed for IN execution that can live between PS
  reexecution.

  @param tmp_columns the items that produce the data for the temp table
  @param subquery_id subquery's identifier (to make "<subquery%d>" name for
                                            EXPLAIN)

  @details
  - Create a temporary table to store the result of the IN subquery. The
    temporary table has one hash index on all its columns.
  - Create a new result sink that sends the result stream of the subquery to
    the temporary table,

  @notice:
    Currently Item_subselect::init() already chooses and creates at parse
    time an engine with a corresponding JOIN to execute the subquery.

  @retval TRUE  if error
  @retval FALSE otherwise
*/

bool subselect_hash_sj_engine::init(List<Item> *tmp_columns, uint subquery_id)
{
  THD *thd= get_thd();
  select_unit *result_sink;
  /* Options to create_tmp_table. */
  ulonglong tmp_create_options= thd->variables.option_bits | TMP_TABLE_ALL_COLUMNS;
                             /* | TMP_TABLE_FORCE_MYISAM; TIMOUR: force MYISAM */

  DBUG_ENTER("subselect_hash_sj_engine::init");

  if (my_bitmap_init_memroot(&non_null_key_parts, tmp_columns->elements,
                            thd->mem_root) ||
      my_bitmap_init_memroot(&partial_match_key_parts, tmp_columns->elements,
                            thd->mem_root))
    DBUG_RETURN(TRUE);

  /*
    Create and initialize a select result interceptor that stores the
    result stream in a temporary table. The temporary table itself is
    managed (created/filled/etc) internally by the interceptor.
  */
/*
  TIMOUR:
  Select a more efficient result sink when we know there is no need to collect
  data statistics.

  if (strategy == COMPLETE_MATCH)
  {
    if (!(result= new select_union))
      DBUG_RETURN(TRUE);
  }
  else if (strategy == PARTIAL_MATCH)
  {
  if (!(result= new select_materialize_with_stats))
    DBUG_RETURN(TRUE);
  }
*/
  if (!(result_sink= new (thd->mem_root) select_materialize_with_stats(thd)))
    DBUG_RETURN(TRUE);
    
  char buf[32];
  LEX_CSTRING name;
  name.length= my_snprintf(buf, sizeof(buf), "<subquery%u>", subquery_id);
  if (!(name.str= (char*) thd->memdup(buf, name.length + 1)))
    DBUG_RETURN(TRUE);

  result_sink->get_tmp_table_param()->materialized_subquery= true;

  if (item->substype() == Item_subselect::IN_SUBS &&
      (item->get_IN_subquery()->is_jtbm_merged))
  {
    result_sink->get_tmp_table_param()->force_not_null_cols= true;
  }
  if (result_sink->create_result_table(thd, tmp_columns, TRUE,
                                       tmp_create_options,
				       &name, TRUE, TRUE, FALSE, 0))
    DBUG_RETURN(TRUE);

  tmp_table= result_sink->table;
  result= result_sink;

  /*
    If the subquery has blobs, or the total key length is bigger than
    some length, or the total number of key parts is more than the
    allowed maximum (currently MAX_REF_PARTS == 32), then the created
    index cannot be used for lookups and we can't use hash semi
    join. If this is the case, delete the temporary table since it
    will not be used, and tell the caller we failed to initialize the
    engine.
  */
  if (tmp_table->s->keys == 0)
  {
    //fprintf(stderr, "Q: %s\n", current_thd->query());
    DBUG_ASSERT(0);
    DBUG_ASSERT(
      tmp_table->s->uniques ||
      tmp_table->key_info->key_length >= tmp_table->file->max_key_length() ||
      tmp_table->key_info->user_defined_key_parts >
      tmp_table->file->max_key_parts());
    free_tmp_table(thd, tmp_table);
    tmp_table= NULL;
    delete result;
    result= NULL;
    DBUG_RETURN(TRUE);
  }

  /*
    Make sure there is only one index on the temp table, and it doesn't have
    the extra key part created when s->uniques > 0.

    NOTE: item have to be Item_in_subselect, because class constructor
    accept Item_in_subselect as the parmeter.
  */
  DBUG_ASSERT(tmp_table->s->keys == 1 &&
              item->get_IN_subquery()->left_expr->cols() ==
              tmp_table->key_info->user_defined_key_parts);

  if (make_semi_join_conds() ||
      /* A unique_engine is used both for complete and partial matching. */
      !(lookup_engine= make_unique_engine()))
    DBUG_RETURN(TRUE);

  /*
    Repeat name resolution for 'cond' since cond is not part of any
    clause of the query, and it is not 'fixed' during JOIN::prepare.
  */
  if (semi_join_conds &&
      semi_join_conds->fix_fields_if_needed(thd, (Item**)&semi_join_conds))
    DBUG_RETURN(TRUE);
  /* Let our engine reuse this query plan for materialization. */
  materialize_join= materialize_engine->join;
  materialize_join->change_result(result, NULL);

  DBUG_RETURN(FALSE);
}


/*
  Create an artificial condition to post-filter those rows matched by index
  lookups that cannot be distinguished by the index lookup procedure.

  @notes
  The need for post-filtering may occur e.g. because of
  truncation. Prepared statements execution requires that fix_fields is
  called for every execution. In order to call fix_fields we need to
  create a Name_resolution_context and a corresponding TABLE_LIST for
  the temporary table for the subquery, so that all column references
  to the materialized subquery table can be resolved correctly.

  @returns
    @retval TRUE  memory allocation error occurred
    @retval FALSE the conditions were created and resolved (fixed)
*/

bool subselect_hash_sj_engine::make_semi_join_conds()
{
  /*
    Table reference for tmp_table that is used to resolve column references
    (Item_fields) to columns in tmp_table.
  */
  TABLE_LIST *tmp_table_ref;
  /* Name resolution context for all tmp_table columns created below. */
  Name_resolution_context *context;
  Item_in_subselect *item_in= item->get_IN_subquery();
  LEX_CSTRING table_name;
  DBUG_ENTER("subselect_hash_sj_engine::make_semi_join_conds");
  DBUG_ASSERT(semi_join_conds == NULL);

  if (!(semi_join_conds= new (thd->mem_root) Item_cond_and(thd)))
    DBUG_RETURN(TRUE);

  if (!(tmp_table_ref= (TABLE_LIST*) thd->alloc(sizeof(TABLE_LIST))))
    DBUG_RETURN(TRUE);

  table_name.str=    tmp_table->alias.c_ptr();
  table_name.length= tmp_table->alias.length(),
  tmp_table_ref->init_one_table(&empty_clex_str, &table_name, NULL, TL_READ);
  tmp_table_ref->table= tmp_table;

  context= new Name_resolution_context(tmp_table_ref);
  semi_join_conds_context= context;

  for (uint i= 0; i < item_in->left_expr->cols(); i++)
  {
    /* New equi-join condition for the current column. */
    Item_func_eq *eq_cond;
    /* Item for the corresponding field from the materialized temp table. */
    Item_field *right_col_item= new (thd->mem_root)
        Item_field(thd, context, tmp_table->field[i]);
    if (right_col_item)
      right_col_item->set_refers_to_temp_table();

    if (!right_col_item ||
        !(eq_cond= new (thd->mem_root)
          Item_func_eq(thd, item_in->left_expr->element_index(i),
                       right_col_item)) ||
        (((Item_cond_and*)semi_join_conds)->add(eq_cond, thd->mem_root)))
    {
      delete semi_join_conds;
      semi_join_conds= NULL;
      DBUG_RETURN(TRUE);
    }
  }
  if (semi_join_conds->fix_fields(thd, (Item**)&semi_join_conds))
    DBUG_RETURN(TRUE);

  DBUG_RETURN(FALSE);
}


/**
  Create a new uniquesubquery engine for the execution of an IN predicate.

  @details
  Create and initialize a new JOIN_TAB, and Table_ref objects to perform
  lookups into the indexed temporary table.

  @retval A new subselect_hash_sj_engine object
  @retval NULL if a memory allocation error occurs
*/

subselect_uniquesubquery_engine*
subselect_hash_sj_engine::make_unique_engine()
{
  Item_in_subselect *item_in= item->get_IN_subquery();
  Item_iterator_row it(item_in->left_expr);
  /* The only index on the temporary table. */
  KEY *tmp_key= tmp_table->key_info;
  JOIN_TAB *tab;

  DBUG_ENTER("subselect_hash_sj_engine::make_unique_engine");

  /*
    Create and initialize the JOIN_TAB that represents an index lookup
    plan operator into the materialized subquery result. Notice that:
    - this JOIN_TAB has no corresponding JOIN (and doesn't need one), and
    - here we initialize only those members that are used by
      subselect_uniquesubquery_engine, so these objects are incomplete.
  */
  if (!(tab= (JOIN_TAB*) thd->alloc(sizeof(JOIN_TAB))))
    DBUG_RETURN(NULL);

  tab->table= tmp_table;
  tab->tab_list= 0;
  tab->preread_init_done= FALSE;
  tab->ref.tmp_table_index_lookup_init(thd, tmp_key, it, FALSE);

  DBUG_RETURN(new (thd->mem_root)
              subselect_uniquesubquery_engine(thd, tab, item_in,
                                              semi_join_conds));
}


subselect_hash_sj_engine::~subselect_hash_sj_engine()
{
  delete lookup_engine;
  delete result;
  if (tmp_table)
    free_tmp_table(thd, tmp_table);
}


int subselect_hash_sj_engine::prepare(THD *thd_arg)
{
  /*
    Create and optimize the JOIN that will be used to materialize
    the subquery if not yet created.
  */
  set_thd(thd_arg);
  return materialize_engine->prepare(thd);
}


/**
  Cleanup performed after each PS execution.

  @details
  Called in the end of JOIN::prepare for PS from Item_subselect::cleanup.
*/

void subselect_hash_sj_engine::cleanup()
{
  enum_engine_type lookup_engine_type= lookup_engine->engine_type();
  is_materialized= FALSE;
  bitmap_clear_all(&non_null_key_parts);
  bitmap_clear_all(&partial_match_key_parts);
  count_partial_match_columns= 0;
  count_null_only_columns= 0;
  strategy= UNDEFINED;
  materialize_engine->cleanup();
  /*
    Restore the original Item_in_subselect engine. This engine is created once
    at parse time and stored across executions, while all other materialization
    related engines are created and chosen for each execution.
  */
  item->get_IN_subquery()->engine= materialize_engine;
  if (lookup_engine_type == SINGLE_COLUMN_ENGINE ||
      lookup_engine_type == TABLE_SCAN_ENGINE ||
      lookup_engine_type == ROWID_MERGE_ENGINE)
  {
    subselect_engine *inner_lookup_engine;
    inner_lookup_engine=
      ((subselect_partial_match_engine*) lookup_engine)->lookup_engine;
    /*
      Partial match engines are recreated for each PS execution inside
      subselect_hash_sj_engine::exec().
    */
    delete lookup_engine;
    lookup_engine= inner_lookup_engine;
  }
  DBUG_ASSERT(lookup_engine->engine_type() == UNIQUESUBQUERY_ENGINE);
  lookup_engine->cleanup();
  result->reset_for_next_ps_execution(); /* Resets the temp table as well. */
  DBUG_ASSERT(tmp_table);
  free_tmp_table(thd, tmp_table);
  tmp_table= NULL;
}


/*
  Get fanout produced by tables specified in the table_map
*/

double get_fanout_with_deps(JOIN *join, table_map tset)
{
  /* Handle the case of "Impossible WHERE" */
  if (join->table_count == 0)
    return 0.0;

  /* First, recursively get all tables we depend on */
  table_map deps_to_check= tset;
  table_map checked_deps= 0;
  table_map further_deps;
  do
  {
    further_deps= 0;
    Table_map_iterator tm_it(deps_to_check);
    int tableno;
    while ((tableno = tm_it.next_bit()) != Table_map_iterator::BITMAP_END)
    {
      /* get tableno's dependency tables that are not in needed_set */
      further_deps |= join->map2table[tableno]->ref.depend_map & ~checked_deps;
    }

    checked_deps |= deps_to_check;
    deps_to_check= further_deps;
  } while (further_deps != 0);

  
  /* Now, walk the join order and calculate the fanout */
  double fanout= 1;
  for (JOIN_TAB *tab= first_top_level_tab(join, WITHOUT_CONST_TABLES); tab;
       tab= next_top_level_tab(join, tab))
  {
    /* 
      Ignore SJM nests. They have tab->table==NULL. There is no point to walk
      inside them, because GROUP BY clause cannot refer to tables from within
      subquery.
    */
    if (!tab->is_sjm_nest() && (tab->table->map & checked_deps) && 
        !tab->emb_sj_nest && 
        tab->records_read != 0)
    {
      fanout *= tab->records_read;
    }
  } 
  return fanout;
}


#if 0
void check_out_index_stats(JOIN *join)
{
  ORDER *order;
  uint n_order_items;

  /*
    First, collect the keys that we can use in each table.
    We can use a key if 
    - all tables refer to it.
  */
  key_map key_start_use[MAX_TABLES];
  key_map key_infix_use[MAX_TABLES];
  table_map key_used=0;
  table_map non_key_used= 0;
  
  bzero(&key_start_use, sizeof(key_start_use)); //psergey-todo: safe initialization!
  bzero(&key_infix_use, sizeof(key_infix_use));
  
  for (order= join->group_list; order; order= order->next)
  {
    Item *item= order->item[0];

    if (item->real_type() == Item::FIELD_ITEM)
    {
      if (item->used_tables() & OUTER_REF_TABLE_BIT)
        continue; /* outside references are like constants for us */

      Field *field= ((Item_field*)item->real_item())->field;
      uint table_no= field->table->tablenr;
      if (!(non_key_used && table_map(1) << table_no) && 
          !field->part_of_key.is_clear_all())
      {
        key_map infix_map= field->part_of_key;
        infix_map.subtract(field->key_start);
        key_start_use[table_no].merge(field->key_start);
        key_infix_use[table_no].merge(infix_map);
        key_used |= table_no;
      }
      continue;
    }
    /* 
      Note: the below will cause clauses like GROUP BY YEAR(date) not to be
      handled. 
    */
    non_key_used |= item->used_tables();
  }
  
  Table_map_iterator tm_it(key_used & ~non_key_used);
  int tableno;
  while ((tableno = tm_it.next_bit()) != Table_map_iterator::BITMAP_END)
  {
    key_map::iterator key_it(key_start_use);
    int keyno;
    while ((keyno = tm_it.next_bit()) != key_map::iterator::BITMAP_END)
    {
      for (order= join->group_list; order; order= order->next)
      {
        Item *item= order->item[0];
        if (item->used_tables() & (table_map(1) << tableno))
        {
          DBUG_ASSERT(item->real_type() == Item::FIELD_ITEM);
        }
      }
      /*
      if (continuation)
      {
        walk through list and find which key parts are occupied;
        // note that the above can't be made any faster.
      }
      else
        use rec_per_key[0];
      
      find out the cardinality.
      check if cardinality decreases if we use it;
      */
    }
  }
}
#endif


/*
  Get an estimate of how many records will be produced after the GROUP BY
  operation.

  @param join           Join we're operating on 
  @param join_op_rows   How many records will be produced by the join
                        operations (this is what join optimizer produces)
  
  @seealso
     See also optimize_semijoin_nests(), grep for "Adjust output cardinality 
     estimates".  Very similar code there that is not joined with this one
     because we operate on different data structs and too much effort is
     needed to abstract them out.

  @return
     Number of records we expect to get after the GROUP BY operation
*/

double get_post_group_estimate(JOIN* join, double join_op_rows)
{
  table_map tables_in_group_list= table_map(0);

  /* Find out which tables are used in GROUP BY list */
  for (ORDER *order= join->group_list_for_estimates; order; order= order->next)
  {
    Item *item= order->item[0];
    table_map item_used_tables= item->used_tables();
    if (item_used_tables & RAND_TABLE_BIT)
    {
      /* Each join output record will be in its own group */
      return join_op_rows;
    }
    tables_in_group_list|= item_used_tables;
  }
  tables_in_group_list &= ~PSEUDO_TABLE_BITS;

  /*
    Use join fanouts to calculate the max. number of records in the group-list
  */
  double fanout_rows[MAX_KEY];
  bzero(&fanout_rows, sizeof(fanout_rows));
  double out_rows;
  
  out_rows= get_fanout_with_deps(join, tables_in_group_list);

#if 0
  /* The following will be needed when making use of index stats: */
  /* 
    Also generate max. number of records for each of the tables mentioned 
    in the group-list. We'll use that a baseline number that we'll try to 
    reduce by using
     - #table-records 
     - index statistics.
  */
  Table_map_iterator tm_it(tables_in_group_list);
  int tableno;
  while ((tableno = tm_it.next_bit()) != Table_map_iterator::BITMAP_END)
  {
    fanout_rows[tableno]= get_fanout_with_deps(join, table_map(1) << tableno);
  }
  
  /*
    Try to bring down estimates using index statistics.
  */
  //check_out_index_stats(join);
#endif

  return out_rows;
}


/**
  Execute a subquery IN predicate via materialization.

  @details
  If needed materialize the subquery into a temporary table, then
  copmpute the predicate via a lookup into this table.

  @retval TRUE  if error
  @retval FALSE otherwise
*/

int subselect_hash_sj_engine::exec()
{
  Item_in_subselect *item_in= item->get_IN_subquery();
  SELECT_LEX *save_select= thd->lex->current_select;
  subselect_partial_match_engine *pm_engine= NULL;
  int res= 0;
  DBUG_ENTER("subselect_hash_sj_engine::exec");

  /*
    Optimize and materialize the subquery during the first execution of
    the subquery predicate.
  */
  thd->lex->current_select= materialize_engine->select_lex;
  /* The subquery should be optimized, and materialized only once. */
  DBUG_ASSERT(materialize_join->optimization_state == JOIN::OPTIMIZATION_DONE &&
              !is_materialized);
  materialize_join->exec();
  if (unlikely((res= MY_TEST(materialize_join->error || thd->is_fatal_error ||
                             thd->is_error()))))
    goto err;

  /*
    TODO:
    - Unlock all subquery tables as we don't need them. To implement this
      we need to add new functionality to JOIN::join_free that can unlock
      all tables in a subquery (and all its subqueries).
    - The temp table used for grouping in the subquery can be freed
      immediately after materialization (yet it's done together with
      unlocking).
  */
  is_materialized= TRUE;
  /*
    If the subquery returned no rows, the temporary table is empty, so we know
    directly that the result of IN is FALSE. We first update the table
    statistics, then we test if the temporary table for the query result is
    empty.
  */
  tmp_table->file->info(HA_STATUS_VARIABLE);
  if (!tmp_table->file->stats.records)
  {
    /* The value of IN will not change during this execution. */
    item_in->reset();
    item_in->make_const();
    item_in->set_first_execution();
    thd->lex->current_select= save_select;
    DBUG_RETURN(FALSE);
  }

  /*
    TIMOUR: The schema-based analysis for partial matching can be done once for
    prepared statement and remembered. It is done here to remove the need to
    save/restore all related variables between each re-execution, thus making
    the code simpler.
  */
  strategy= get_strategy_using_schema();
  /* This call may discover that we don't need partial matching at all. */
  strategy= get_strategy_using_data();
  if (strategy == PARTIAL_MATCH)
  {
    uint count_pm_keys; /* Total number of keys needed for partial matching. */
    MY_BITMAP *nn_key_parts= NULL; /* Key parts of the only non-NULL index. */
    uint count_non_null_columns= 0; /* Number of columns in nn_key_parts. */
    bool has_covering_null_row;
    bool has_covering_null_columns;
    select_materialize_with_stats *result_sink=
      (select_materialize_with_stats *) result;
    uint field_count= tmp_table->s->fields;

    if (count_partial_match_columns < field_count)
    {
      nn_key_parts= &non_null_key_parts;
      count_non_null_columns= bitmap_bits_set(nn_key_parts);
    }
    has_covering_null_row= (result_sink->get_max_nulls_in_row() == field_count);
    has_covering_null_columns= (count_non_null_columns +
                                count_null_only_columns == field_count);

    if (has_covering_null_row && has_covering_null_columns)
    {
      /*
        The whole table consist of only NULL values. The result of IN is
        a constant UNKNOWN.
      */
      DBUG_ASSERT(tmp_table->file->stats.records == 1);
      item_in->value= 0;
      item_in->null_value= 1;
      item_in->make_const();
      item_in->set_first_execution();
      res= 0;
      strategy= CONST_RETURN_NULL;
      goto err;
    }

    if (has_covering_null_row)
    {
      DBUG_ASSERT(count_partial_match_columns == field_count);
      count_pm_keys= 0;
    }
    else if (has_covering_null_columns)
      count_pm_keys= 1;
    else
      count_pm_keys= count_partial_match_columns - count_null_only_columns +
                     (nn_key_parts ? 1 : 0);

    choose_partial_match_strategy(field_count, MY_TEST(nn_key_parts),
                                  has_covering_null_row,
                                  &partial_match_key_parts);
    DBUG_ASSERT(strategy == SINGLE_COLUMN_MATCH ||
                strategy == PARTIAL_MATCH_MERGE ||
                strategy == PARTIAL_MATCH_SCAN);
    if (strategy == SINGLE_COLUMN_MATCH)
    {
      if (!(pm_engine= new subselect_single_column_match_engine(thd,
              (subselect_uniquesubquery_engine*) lookup_engine, tmp_table,
              item, result, semi_join_conds->argument_list(),
              has_covering_null_row, has_covering_null_columns,
              count_columns_with_nulls)) ||
          pm_engine->prepare(thd))
      {
        /* This is an irrecoverable error. */
        res= 1;
        goto err;
      }
    }
    if (strategy == PARTIAL_MATCH_MERGE)
    {
      pm_engine=
        (new (thd->mem_root)
         subselect_rowid_merge_engine(thd,
                                      (subselect_uniquesubquery_engine*)
                                      lookup_engine, tmp_table,
                                      count_pm_keys,
                                      has_covering_null_row,
                                      has_covering_null_columns,
                                      count_columns_with_nulls,
                                      item, result,
                                      semi_join_conds->argument_list()));
      if (!pm_engine ||
          pm_engine->prepare(thd) ||
          ((subselect_rowid_merge_engine*) pm_engine)->
            init(nn_key_parts, &partial_match_key_parts))
      {
        /*
          The call to init() would fail if there was not enough memory
          to allocate all buffers for the rowid merge strategy. In
          this case revert to table scanning which doesn't need any
          big buffers.
        */
        delete pm_engine;
        pm_engine= NULL;
        strategy= PARTIAL_MATCH_SCAN;
      }
    }
    if (strategy == PARTIAL_MATCH_SCAN)
    {
      if (!(pm_engine=
            (new (thd->mem_root)
             subselect_table_scan_engine(thd,
                                         (subselect_uniquesubquery_engine*)
                                         lookup_engine, tmp_table,
                                         item, result,
                                         semi_join_conds->argument_list(),
                                         has_covering_null_row,
                                         has_covering_null_columns,
                                         count_columns_with_nulls))) ||
          pm_engine->prepare(thd))
      {
        /* This is an irrecoverable error. */
        res= 1;
        goto err;
      }
    }
  }

  if (pm_engine)
    lookup_engine= pm_engine;
  item_in->change_engine(lookup_engine);

err:
  item_in->get_materialization_tracker()->report_exec_strategy(strategy);
  thd->lex->current_select= save_select;
  DBUG_RETURN(res);
}


/**
  Print the state of this engine into a string for debugging and views.
*/

void subselect_hash_sj_engine::print(String *str, enum_query_type query_type)
{
  str->append(STRING_WITH_LEN(" <materialize> ("));
  materialize_engine->print(str, query_type);
  str->append(STRING_WITH_LEN(" ), "));

  if (lookup_engine)
    lookup_engine->print(str, query_type);
  else
    str->append(STRING_WITH_LEN(
           "<engine selected at execution time>"
         ));
}

bool subselect_hash_sj_engine::fix_length_and_dec(Item_cache** row)
{
  DBUG_ASSERT(FALSE);
  return FALSE;
}

void subselect_hash_sj_engine::exclude()
{
  DBUG_ASSERT(FALSE);
}

bool subselect_hash_sj_engine::no_tables() const
{
  DBUG_ASSERT(FALSE);
  return FALSE;
}

bool subselect_hash_sj_engine::change_result(Item_subselect *si,
                                             select_result_interceptor *res,
                                             bool temp __attribute__((unused)))
{
  DBUG_ASSERT(FALSE);
  return TRUE;
}


Ordered_key::Ordered_key(uint keyid_arg, TABLE *tbl_arg, Item *search_key_arg,
                         ha_rows null_count_arg, ha_rows min_null_row_arg,
                         ha_rows max_null_row_arg, uchar *row_num_to_rowid_arg)
  : keyid(keyid_arg), tbl(tbl_arg), search_key(search_key_arg),
    row_num_to_rowid(row_num_to_rowid_arg), null_count(null_count_arg)
{
  DBUG_ASSERT(tbl->file->stats.records > null_count);
  key_buff_elements= tbl->file->stats.records - null_count;
  cur_key_idx= HA_POS_ERROR;

  DBUG_ASSERT((null_count && min_null_row_arg && max_null_row_arg) ||
              (!null_count && !min_null_row_arg && !max_null_row_arg));
  if (null_count)
  {
    /* The counters are 1-based, for key access we need 0-based indexes. */
    min_null_row= min_null_row_arg - 1;
    max_null_row= max_null_row_arg - 1;
  }
  else
    min_null_row= max_null_row= 0;
}


Ordered_key::~Ordered_key()
{
  my_free(key_buff);
  my_bitmap_free(&null_key);
}


/*
  Cleanup that needs to be done for each PS (re)execution.
*/

void Ordered_key::cleanup()
{
  /*
    Currently these keys are recreated for each PS re-execution, thus
    there is nothing to cleanup, the whole object goes away after execution
    is over. All handler related initialization/deinitialization is done by
    the parent subselect_rowid_merge_engine object.
  */
}


/*
  Initialize a multi-column index.
*/

bool Ordered_key::init(MY_BITMAP *columns_to_index)
{
  THD *thd= tbl->in_use;
  uint cur_key_col= 0;
  Item_field *cur_tmp_field;
  Item_func_lt *fn_less_than;

  key_column_count= bitmap_bits_set(columns_to_index);
  key_columns= (Item_field**) thd->alloc(key_column_count *
                                         sizeof(Item_field*));
  compare_pred= (Item_func_lt**) thd->alloc(key_column_count *
                                            sizeof(Item_func_lt*));

  if (!key_columns || !compare_pred)
    return TRUE; /* Revert to table scan partial match. */

  for (uint i= 0; i < columns_to_index->n_bits; i++)
  {
    if (!bitmap_is_set(columns_to_index, i))
      continue;
    cur_tmp_field= new (thd->mem_root) Item_field(thd, tbl->field[i]);
    /* Create the predicate (tmp_column[i] < outer_ref[i]). */
    fn_less_than= new (thd->mem_root) Item_func_lt(thd, cur_tmp_field,
                                   search_key->element_index(i));
    fn_less_than->fix_fields(thd, (Item**) &fn_less_than);
    key_columns[cur_key_col]= cur_tmp_field;
    compare_pred[cur_key_col]= fn_less_than;
    ++cur_key_col;
  }

  if (alloc_keys_buffers())
  {
    /* TIMOUR revert to partial match via table scan. */
    return TRUE;
  }
  return FALSE;
}


/*
  Initialize a single-column index.
*/

bool Ordered_key::init(int col_idx)
{
  THD *thd= tbl->in_use;

  key_column_count= 1;

  // TIMOUR: check for mem allocation err, revert to scan

  key_columns= (Item_field**) thd->alloc(sizeof(Item_field*));
  compare_pred= (Item_func_lt**) thd->alloc(sizeof(Item_func_lt*));

  key_columns[0]= new (thd->mem_root) Item_field(thd, tbl->field[col_idx]);
  /* Create the predicate (tmp_column[i] < outer_ref[i]). */
  compare_pred[0]= new (thd->mem_root) Item_func_lt(thd, key_columns[0],
                                    search_key->element_index(col_idx));
  compare_pred[0]->fix_fields(thd, (Item**)&compare_pred[0]);

  if (alloc_keys_buffers())
  {
    /* TIMOUR revert to partial match via table scan. */
    return TRUE;
  }
  return FALSE;
}


/*
  Allocate the buffers for both the row number, and the NULL-bitmap indexes.
*/

bool Ordered_key::alloc_keys_buffers()
{
  DBUG_ASSERT(key_buff_elements > 0);

  if (!(key_buff= (rownum_t*) my_malloc(PSI_INSTRUMENT_ME,
         static_cast<size_t>(key_buff_elements * sizeof(rownum_t)),
         MYF(MY_WME | MY_THREAD_SPECIFIC))))
    return TRUE;

  /*
    TIMOUR: it is enough to create bitmaps with size
    (max_null_row - min_null_row), and then use min_null_row as
    lookup offset.
  */
  /* Notice that max_null_row is max array index, we need count, so +1. */
  if (my_bitmap_init(&null_key, NULL, (uint)(max_null_row + 1)))
    return TRUE;

  cur_key_idx= HA_POS_ERROR;

  return FALSE;
}


/*
  Quick sort comparison function that compares two rows of the same table
  indentfied with their row numbers.

  @retval -1
  @retval  0
  @retval +1
*/

int
Ordered_key::cmp_keys_by_row_data(const ha_rows a, const ha_rows b) const
{
  uchar *rowid_a, *rowid_b;
  int error;
  int cmp_res;
  /* The length in bytes of the rowids (positions) of tmp_table. */
  uint rowid_length= tbl->file->ref_length;

  if (a == b)
    return 0;
  /* Get the corresponding rowids. */
  rowid_a= row_num_to_rowid + a * rowid_length;
  rowid_b= row_num_to_rowid + b * rowid_length;
  /* Fetch the rows for comparison. */
  if (unlikely((error= tbl->file->ha_rnd_pos(tbl->record[0], rowid_a))))
  {
    /* purecov: begin inspected */
    tbl->file->print_error(error, MYF(ME_FATAL));  // Sets fatal_error
    return 0;
    /* purecov: end */
  }
  if (unlikely((error= tbl->file->ha_rnd_pos(tbl->record[1], rowid_b))))
  {
    /* purecov: begin inspected */
    tbl->file->print_error(error, MYF(ME_FATAL));  // Sets fatal_error
    return 0;
    /* purecov: end */
  }    
  /*
    Compare the two rows by the corresponding values of the indexed
    columns.
  */
  for (uint i= 0; i < key_column_count; i++)
  {
    Field *cur_field= key_columns[i]->field;
    if ((cmp_res= cur_field->cmp_offset(tbl->s->rec_buff_length)))
      return (cmp_res > 0 ? 1 : -1);
  }
  return 0;
}


int Ordered_key::cmp_keys_by_row_data_and_rownum(void *key_, const void *a_,
                                                 const void *b_)
{
  Ordered_key *key= static_cast<Ordered_key *>(key_);
  const rownum_t *a= static_cast<const rownum_t *>(a_);
  const rownum_t *b= static_cast<const rownum_t *>(b_);
  /* The result of comparing the two keys according to their row data. */
  int cmp_row_res= key->cmp_keys_by_row_data(*a, *b);
  if (cmp_row_res)
    return cmp_row_res;
  return (*a < *b) ? -1 : (*a > *b) ? 1 : 0;
}


bool Ordered_key::sort_keys()
{
  if (tbl->file->ha_rnd_init_with_error(0))
    return TRUE;
  my_qsort2(key_buff, (size_t) key_buff_elements, sizeof(rownum_t),
            &cmp_keys_by_row_data_and_rownum, (void *) this);
  /* Invalidate the current row position. */
  cur_key_idx= HA_POS_ERROR;
  tbl->file->ha_rnd_end();
  return FALSE;
}


/*
  The fraction of rows that do not contain NULL in the columns indexed by
  this key.

  @retval  1  if there are no NULLs
  @retval  0  if only NULLs
*/

inline double Ordered_key::null_selectivity() const
{
  /* We should not be processing empty tables. */
  DBUG_ASSERT(tbl->file->stats.records);
  return (1 - (double) null_count / (double) tbl->file->stats.records);
}


/*
  Compare the value(s) of the current key in 'search_key' with the
  data of the current table record.

  @notes The comparison result follows from the way compare_pred
  is created in Ordered_key::init. Currently compare_pred compares
  a field in of the current row with the corresponding Item that
  contains the search key.

  @param row_num  Number of the row (not index in the key_buff array)

  @retval -1  if (current row  < search_key)
  @retval  0  if (current row == search_key)
  @retval +1  if (current row  > search_key)
*/

int Ordered_key::cmp_key_with_search_key(rownum_t row_num)
{
  /* The length in bytes of the rowids (positions) of tmp_table. */
  uint rowid_length= tbl->file->ref_length;
  uchar *cur_rowid= row_num_to_rowid + row_num * rowid_length;
  int error;
  int cmp_res;

  if (unlikely((error= tbl->file->ha_rnd_pos(tbl->record[0], cur_rowid))))
  {
    /* purecov: begin inspected */
    tbl->file->print_error(error, MYF(ME_FATAL));  // Sets fatal_error
    return 0;
    /* purecov: end */
  }

  for (uint i= 0; i < key_column_count; i++)
  {
    cmp_res= compare_pred[i]->get_comparator()->compare();
    /* Unlike Arg_comparator::compare_row() here there should be no NULLs. */
    DBUG_ASSERT(!compare_pred[i]->null_value);
    if (cmp_res)
      return (cmp_res > 0 ? 1 : -1);
  }
  return 0;
}


/*
  Find a key in a sorted array of keys via binary search.

  see create_subq_in_equalities()
*/

bool Ordered_key::lookup()
{
  DBUG_ASSERT(key_buff_elements);

  ha_rows lo= 0;
  ha_rows hi= key_buff_elements - 1;
  ha_rows mid;
  int cmp_res;

  while (lo <= hi)
  {
    mid= lo + (hi - lo) / 2;
    cmp_res= cmp_key_with_search_key(key_buff[mid]);
    /*
      In order to find the minimum match, check if the pevious element is
      equal or smaller than the found one. If equal, we need to search further
      to the left.
    */
    if (!cmp_res && mid > 0)
      cmp_res= !cmp_key_with_search_key(key_buff[mid - 1]) ? 1 : 0;

    if (cmp_res == -1)
    {
      /* row[mid] < search_key */
      lo= mid + 1;
    }
    else if (cmp_res == 1)
    {
      /* row[mid] > search_key */
      if (!mid)
        goto not_found;
      hi= mid - 1;
    }
    else
    {
      /* row[mid] == search_key */
      cur_key_idx= mid;
      return TRUE;
    }
  }
not_found:
  cur_key_idx= HA_POS_ERROR;
  return FALSE;
}


/*
  Move the current index pointer to the next key with the same column
  values as the current key. Since the index is sorted, all such keys
  are contiguous.
*/

bool Ordered_key::next_same()
{
  DBUG_ASSERT(key_buff_elements);

  if (cur_key_idx < key_buff_elements - 1)
  {
    /*
      TIMOUR:
      The below is quite inefficient, since as a result we will fetch every
      row (except the last one) twice. There must be a more efficient way,
      e.g. swapping record[0] and record[1], and reading only the new record.
    */
    if (!cmp_keys_by_row_data(key_buff[cur_key_idx], key_buff[cur_key_idx + 1]))
    {
      ++cur_key_idx;
      return TRUE;
    }
  }
  return FALSE;
}


void Ordered_key::print(String *str) const
{
  uint i;

  /* We have to pre-allocate string as we are using qs_append() */
  if (str->alloc(str->length() +
                 5+10+4+ (NAME_LEN+2)*key_column_count+
                 20+11+21+10+FLOATING_POINT_BUFFER*3+50
                 ))
      return;
  str->append(STRING_WITH_LEN("{idx="));
  str->qs_append(keyid);
  str->append(STRING_WITH_LEN(", ("));
  for (i= 0; i < key_column_count ; i++)
  {
    str->append(&key_columns[i]->field->field_name);
    str->append(STRING_WITH_LEN(", "));
  }
  if (key_column_count)
    str->length(str->length() - 2);
  str->append(STRING_WITH_LEN("), "));

  str->append(STRING_WITH_LEN("null_bitmap: (bits="));
  str->qs_append(null_key.n_bits);
  str->append(STRING_WITH_LEN(", nulls= "));
  str->qs_append((double)null_count);
  str->append(STRING_WITH_LEN(", min_null= "));
  str->qs_append((double)min_null_row);
  str->append(STRING_WITH_LEN(", max_null= "));
  str->qs_append((double)max_null_row);
  str->append(STRING_WITH_LEN("), "));

  str->append('}');
}


subselect_partial_match_engine::subselect_partial_match_engine(
  THD *thd_arg,
  subselect_uniquesubquery_engine *engine_arg,
  TABLE *tmp_table_arg, Item_subselect *item_arg,
  select_result_interceptor *result_arg,
  List<Item> *equi_join_conds_arg,
  bool has_covering_null_row_arg,
  bool has_covering_null_columns_arg,
  uint count_columns_with_nulls_arg)
  :subselect_engine(item_arg, result_arg),
   tmp_table(tmp_table_arg), lookup_engine(engine_arg),
   equi_join_conds(equi_join_conds_arg),
   has_covering_null_row(has_covering_null_row_arg),
   has_covering_null_columns(has_covering_null_columns_arg),
   count_columns_with_nulls(count_columns_with_nulls_arg)
{
  thd= thd_arg;
}


int subselect_partial_match_engine::exec()
{
  Item_in_subselect *item_in= item->get_IN_subquery();
  int lookup_res;
  DBUG_ASSERT(thd);

  DBUG_ASSERT(!(item_in->left_expr_has_null() &&
                item_in->is_top_level_item()));

  Subq_materialization_tracker *tracker= item_in->get_materialization_tracker();
  tracker->increment_loops_count();

  if (!item_in->left_expr_has_null())
  {
    /* Try to find a matching row by index lookup. */
    if (lookup_engine->copy_ref_key(false))
    {
      /* The result is FALSE based on the outer reference. */
      item_in->value= 0;
      item_in->null_value= 0;
      return 0;
    }
    else
    {
      /* Search for a complete match. */
      tracker->increment_index_lookups();
      if ((lookup_res= lookup_engine->index_lookup()))
      {
        /* An error occurred during lookup(). */
        item_in->value= 0;
        item_in->null_value= 0;
        return lookup_res;
      }
      else if (item_in->value || !count_columns_with_nulls)
      {
        /*
          A complete match was found, the result of IN is TRUE.
          If no match was found, and there are no NULLs in the materialized
          subquery, then the result is guaranteed to be false because this
          branch is executed when the outer reference has no NULLs as well.
          Notice: (this->item == lookup_engine->item)
        */
        return 0;
      }
    }
  }

  if (has_covering_null_row)
  {
    /*
      If there is a NULL-only row that covers all columns the result of IN
      is UNKNOWN. 
    */
    item_in->value= 0;
    /*
      TIMOUR: which one is the right way to propagate an UNKNOWN result?
      Should we also set empty_result_set= FALSE; ???
    */
    //item_in->was_null= 1;
    item_in->null_value= 1;
    return 0;
  }

  /*
    There is no complete match. Look for a partial match (UNKNOWN result), or
    no match (FALSE).
  */
  if (tmp_table->file->inited)
    tmp_table->file->ha_index_end();

  tracker->increment_partial_matches();
  if (partial_match())
  {
    /* The result of IN is UNKNOWN. */
    item_in->value= 0;
    /*
      TIMOUR: which one is the right way to propagate an UNKNOWN result?
      Should we also set empty_result_set= FALSE; ???
    */
    //item_in->was_null= 1;
    item_in->null_value= 1;
  }
  else
  {
    /* The result of IN is FALSE. */
    item_in->value= 0;
    /*
      TIMOUR: which one is the right way to propagate an UNKNOWN result?
      Should we also set empty_result_set= FALSE; ???
    */
    //item_in->was_null= 0;
    item_in->null_value= 0;
  }

  return 0;
}


void subselect_partial_match_engine::print(String *str,
                                           enum_query_type query_type)
{
  /*
    Should never be called as the actual engine cannot be known at query
    optimization time.
    DBUG_ASSERT(FALSE);
  */
}


/*
  @param non_null_key_parts  
  @param partial_match_key_parts  A union of all single-column NULL key parts.

  @retval FALSE  the engine was initialized successfully
  @retval TRUE   there was some (memory allocation) error during initialization,
                 such errors should be interpreted as revert to other strategy
*/

bool
subselect_rowid_merge_engine::init(MY_BITMAP *non_null_key_parts,
                                   MY_BITMAP *partial_match_key_parts)
{
  THD *thd= get_thd();
  /* The length in bytes of the rowids (positions) of tmp_table. */
  uint rowid_length= tmp_table->file->ref_length;
  ha_rows row_count= tmp_table->file->stats.records;
  rownum_t cur_rownum= 0;
  select_materialize_with_stats *result_sink=
    (select_materialize_with_stats *) result;
  uint cur_keyid= 0;
  Item *left= item->get_IN_subquery()->left_exp();
  int error;

  if (merge_keys_count == 0)
  {
    DBUG_ASSERT(bitmap_bits_set(partial_match_key_parts) == 0 ||
                has_covering_null_row);
    /* There is nothing to initialize, we will only do regular lookups. */
    return FALSE;
  }

  /*
    If all nullable columns contain only NULLs, there must be one index
    over all non-null columns.
  */
  DBUG_ASSERT(!has_covering_null_columns ||
              (has_covering_null_columns &&
               merge_keys_count == 1 && non_null_key_parts));
  /*
    Allocate buffers to hold the merged keys and the mapping between rowids and
    row numbers. All small buffers are allocated in the runtime memroot. Big
    buffers are allocated from the OS via malloc.
  */
  if (!(merge_keys= (Ordered_key**) thd->alloc(merge_keys_count *
                                               sizeof(Ordered_key*))) ||
      !(null_bitmaps= (MY_BITMAP**) thd->alloc(merge_keys_count *
                                               sizeof(MY_BITMAP*))) ||
      !(row_num_to_rowid= (uchar*) my_malloc(PSI_INSTRUMENT_ME,
                              static_cast<size_t>(row_count * rowid_length),
                              MYF(MY_WME | MY_THREAD_SPECIFIC))))
    return TRUE;

  /* Create the only non-NULL key if there is any. */
  if (non_null_key_parts)
  {
    non_null_key= (new (thd->mem_root)
                   Ordered_key(cur_keyid, tmp_table, left,
                               0, 0, 0, row_num_to_rowid));
    if (non_null_key->init(non_null_key_parts))
      return TRUE;
    merge_keys[cur_keyid]= non_null_key;
    merge_keys[cur_keyid]->first();
    ++cur_keyid;
  }

  /*
    If all nullable columns contain NULLs, the only key that is needed is the
    only non-NULL key that is already created above.
  */
  if (!has_covering_null_columns)
  {
    if (my_bitmap_init_memroot(&matching_keys, merge_keys_count, thd->mem_root) ||
        my_bitmap_init_memroot(&matching_outer_cols, merge_keys_count, thd->mem_root))
      return TRUE;

    /*
      Create one single-column NULL-key for each column in
      partial_match_key_parts.
    */
    for (uint i= 0; i < partial_match_key_parts->n_bits; i++)
    {
      /* Skip columns that have no NULLs, or contain only NULLs. */
      if (!bitmap_is_set(partial_match_key_parts, i) ||
          result_sink->get_null_count_of_col(i) == row_count)
        continue;

      merge_keys[cur_keyid]= new (thd->mem_root)
          Ordered_key(cur_keyid, tmp_table,
                      left->element_index(i),
                      result_sink->get_null_count_of_col(i),
                      result_sink->get_min_null_of_col(i),
                      result_sink->get_max_null_of_col(i),
                      row_num_to_rowid);
      if (merge_keys[cur_keyid]->init(i))
        return TRUE;
      merge_keys[cur_keyid]->first();
      ++cur_keyid;
    }
  }
  DBUG_ASSERT(cur_keyid == merge_keys_count);

  /* Populate the indexes with data from the temporary table. */
  if (unlikely(tmp_table->file->ha_rnd_init_with_error(1)))
    return TRUE;
  tmp_table->file->extra_opt(HA_EXTRA_CACHE,
                             current_thd->variables.read_buff_size);
  tmp_table->null_row= 0;
  while (TRUE)
  {
    error= tmp_table->file->ha_rnd_next(tmp_table->record[0]);

    if (error == HA_ERR_ABORTED_BY_USER)
      break;
    /*
      This is a temp table that we fully own, there should be no other
      cause to stop the iteration than EOF.
    */
    DBUG_ASSERT(!error || error == HA_ERR_END_OF_FILE);
    if (unlikely(error == HA_ERR_END_OF_FILE))
    {
      DBUG_ASSERT(cur_rownum == tmp_table->file->stats.records);
      break;
    }

    /*
      Save the position of this record in the row_num -> rowid mapping.
    */
    tmp_table->file->position(tmp_table->record[0]);
    memcpy(row_num_to_rowid + cur_rownum * rowid_length,
           tmp_table->file->ref, rowid_length);

    /* Add the current row number to the corresponding keys. */
    if (non_null_key)
    {
      /* By definition there are no NULLs in the non-NULL key. */
      non_null_key->add_key(cur_rownum);
    }

    for (uint i= (non_null_key ? 1 : 0); i < merge_keys_count; i++)
    {
      /*
        Check if the first and only indexed column contains NULL in the current
        row, and add the row number to the corresponding key.
      */
      if (merge_keys[i]->get_field(0)->is_null())
        merge_keys[i]->set_null(cur_rownum);
      else
        merge_keys[i]->add_key(cur_rownum);
    }
    ++cur_rownum;
  }

  tmp_table->file->ha_rnd_end();

  /* Sort all the keys by their NULL selectivity. */
  my_qsort(merge_keys, merge_keys_count, sizeof(Ordered_key*),
           (qsort_cmp) cmp_keys_by_null_selectivity);

  /* Sort the keys in each of the indexes. */
  for (uint i= 0; i < merge_keys_count; i++)
    if (merge_keys[i]->sort_keys())
      return TRUE;

  if (init_queue(&pq, merge_keys_count, 0, FALSE,
                 subselect_rowid_merge_engine::cmp_keys_by_cur_rownum, NULL,
                 0, 0))
    return TRUE;

  item->get_IN_subquery()->get_materialization_tracker()->
      report_partial_merge_keys(merge_keys, merge_keys_count);
  return FALSE;
}


subselect_rowid_merge_engine::~subselect_rowid_merge_engine()
{
  /* None of the resources below is allocated if there are no ordered keys. */
  if (merge_keys_count)
  {
    my_free(row_num_to_rowid);
    for (uint i= 0; i < merge_keys_count; i++)
      delete merge_keys[i];
    delete_queue(&pq);
    if (tmp_table->file->inited == handler::RND)
      tmp_table->file->ha_rnd_end();
  }
}


void subselect_rowid_merge_engine::cleanup()
{
}


/*
  Quick sort comparison function to compare keys in order of decreasing bitmap
  selectivity, so that the most selective keys come first.

  @param  k1 first key to compare
  @param  k2 second key to compare

  @retval  1  if k1 is less selective than k2
  @retval  0  if k1 is equally selective as k2
  @retval -1  if k1 is more selective than k2
*/

int subselect_rowid_merge_engine::cmp_keys_by_null_selectivity(const void *k1_,
                                                               const void *k2_)
{
  auto k1= static_cast<const Ordered_key *const *>(k1_);
  auto k2= static_cast<const Ordered_key *const *>(k2_);
  double k1_sel= (*k1)->null_selectivity();
  double k2_sel= (*k2)->null_selectivity();
  if (k1_sel < k2_sel)
    return 1;
  if (k1_sel > k2_sel)
    return -1;
  return 0;
}


/*
*/

int subselect_rowid_merge_engine::cmp_keys_by_cur_rownum(void *,
                                                         const void *k1_,
                                                         const void *k2_)
{
  auto k1= static_cast<const Ordered_key *>(k1_);
  auto k2= static_cast<const Ordered_key *>(k2_);
  rownum_t r1= k1->current();
  rownum_t r2= k2->current();

  return (r1 < r2) ? -1 : (r1 > r2) ? 1 : 0;
}


/*
  Check if certain table row contains a NULL in all columns for which there is
  no match in the corresponding value index.

  @note
  There is no need to check the columns that contain only NULLs, because
  those are guaranteed to match.

  @retval TRUE if a NULL row exists
  @retval FALSE otherwise
*/

bool subselect_rowid_merge_engine::test_null_row(rownum_t row_num)
{
  Ordered_key *cur_key;
  for (uint i = 0; i < merge_keys_count; i++)
  {
    cur_key= merge_keys[i];
    if (bitmap_is_set(&matching_keys, cur_key->get_keyid()))
    {
      /*
        The key 'i' (with id 'cur_keyid') already matches a value in row
        'row_num', thus we skip it as it can't possibly match a NULL.
      */
      continue;
    }
    if (!cur_key->is_null(row_num))
      return FALSE;
  }
  return TRUE;
}


/**
  Test if a subset of NULL-able columns contains a row of NULLs.
  @retval TRUE  if such a row exists
  @retval FALSE no complementing null row
*/

bool subselect_rowid_merge_engine::
exists_complementing_null_row(MY_BITMAP *keys_to_complement)
{
  rownum_t highest_min_row= 0;
  rownum_t lowest_max_row= UINT_MAX;
  uint count_null_keys, i;
  Ordered_key *cur_key;

  if (!count_columns_with_nulls)
  {
    /*
      If there are both NULLs and non-NUll values in the outer reference, and
      the subquery contains no NULLs, a complementing NULL row cannot exist.
    */
    return FALSE;
  }

  for (i= (non_null_key ? 1 : 0), count_null_keys= 0; i < merge_keys_count; i++)
  {
    cur_key= merge_keys[i];
    if (bitmap_is_set(keys_to_complement, cur_key->get_keyid()))
      continue;
    if (!cur_key->get_null_count())
    {
      /* If there is column without NULLs, there cannot be a partial match. */
      return FALSE;
    }
    if (cur_key->get_min_null_row() > highest_min_row)
      highest_min_row= cur_key->get_min_null_row();
    if (cur_key->get_max_null_row() < lowest_max_row)
      lowest_max_row= cur_key->get_max_null_row();
    null_bitmaps[count_null_keys++]= cur_key->get_null_key();
  }

  if (lowest_max_row < highest_min_row)
  {
    /* The intersection of NULL rows is empty. */
    return FALSE;
  }

  return bitmap_exists_intersection(null_bitmaps,
                                    count_null_keys,
                                    (uint)highest_min_row,
                                    (uint)lowest_max_row);
}


/*
  @retval TRUE  there is a partial match (UNKNOWN)
  @retval FALSE  there is no match at all (FALSE)
*/

bool subselect_rowid_merge_engine::partial_match()
{
  Ordered_key *min_key; /* Key that contains the current minimum position. */
  rownum_t min_row_num; /* Current row number of min_key. */
  Ordered_key *cur_key;
  rownum_t cur_row_num;
  uint count_nulls_in_search_key= 0;
  uint max_null_in_any_row=
    ((select_materialize_with_stats *) result)->get_max_nulls_in_row();
  bool res= FALSE;

  /* If there is a non-NULL key, it must be the first key in the keys array. */
  DBUG_ASSERT(!non_null_key || (non_null_key && merge_keys[0] == non_null_key));
  /* The prioryty queue for keys must be empty. */
  DBUG_ASSERT(!pq.elements);

  /* All data accesses during execution are via handler::ha_rnd_pos() */
  if (unlikely(tmp_table->file->ha_rnd_init_with_error(0)))
  {
    res= FALSE;
    goto end;
  }

  /* Check if there is a match for the columns of the only non-NULL key. */
  if (non_null_key && !non_null_key->lookup())
  {
    res= FALSE;
    goto end;
  }

  /*
    If all nullable columns contain only NULLs, then there is a guaranteed
    partial match, and we don't need to search for a matching row.
  */
  if (has_covering_null_columns)
  {
    res= TRUE;
    goto end;
  }

  if (non_null_key)
    queue_insert(&pq, (uchar *) non_null_key);
  /*
    Do not add the non_null_key, since it was already processed above.
  */
  bitmap_clear_all(&matching_outer_cols);
  for (uint i= MY_TEST(non_null_key); i < merge_keys_count; i++)
  {
    DBUG_ASSERT(merge_keys[i]->get_column_count() == 1);
    if (merge_keys[i]->get_search_key(0)->null_value)
    {
      ++count_nulls_in_search_key;
      bitmap_set_bit(&matching_outer_cols, merge_keys[i]->get_keyid());
    }
    else if (merge_keys[i]->lookup())
      queue_insert(&pq, (uchar *) merge_keys[i]);
  }

  /*
    If the outer reference consists of only NULLs, or if it has NULLs in all
    nullable columns (above we guarantee there is a match for the non-null
    coumns), the result is UNKNOWN.
  */
  if (count_nulls_in_search_key == merge_keys_count - MY_TEST(non_null_key))
  {
    res= TRUE;
    goto end;
  }

  /*
    If the outer row has NULLs in some columns, and
    there is no match for any of the remaining columns, and
    there is a subquery row with NULLs in all unmatched columns,
    then there is a partial match, otherwise the result is FALSE.
  */
  if (count_nulls_in_search_key && !pq.elements)
  {
    DBUG_ASSERT(!non_null_key);
    /*
      Check if the intersection of all NULL bitmaps of all keys that
      are not in matching_outer_cols is non-empty.
    */
    res= exists_complementing_null_row(&matching_outer_cols);
    goto end;
  }

  /*
    If there is no NULL (sub)row that covers all NULL columns, and there is no
    match for any of the NULL columns, the result is FALSE. Notice that if there
    is a non-null key, and there is only one matching key, the non-null key is
    the matching key. This is so, because this method returns FALSE if the
    non-null key doesn't have a match.
  */
  if (!count_nulls_in_search_key &&
      (!pq.elements ||
       (pq.elements == 1 && non_null_key &&
        max_null_in_any_row < merge_keys_count-1)))
  {
    if (!pq.elements)
    {
      DBUG_ASSERT(!non_null_key);
      /*
        The case of a covering null row is handled by
        subselect_partial_match_engine::exec()
      */
      DBUG_ASSERT(max_null_in_any_row != tmp_table->s->fields);
    }
    res= FALSE;
    goto end;
  }

  DBUG_ASSERT(pq.elements);

  min_key= (Ordered_key*) queue_remove_top(&pq);
  min_row_num= min_key->current();
  bitmap_set_bit(&matching_keys, min_key->get_keyid());
  bitmap_union(&matching_keys, &matching_outer_cols);
  if (min_key->next_same())
    queue_insert(&pq, (uchar *) min_key);

  if (pq.elements == 0)
  {
    /*
      Check the only matching row of the only key min_key for NULL matches
      in the other columns.
    */
    res= test_null_row(min_row_num);
    goto end;
  }

  while (TRUE)
  {
    cur_key= (Ordered_key*) queue_remove_top(&pq);
    cur_row_num= cur_key->current();

    if (cur_row_num == min_row_num)
      bitmap_set_bit(&matching_keys, cur_key->get_keyid());
    else
    {
      /* Follows from the correct use of priority queue. */
      DBUG_ASSERT(cur_row_num > min_row_num);
      if (test_null_row(min_row_num))
      {
        res= TRUE;
        goto end;
      }
      else
      {
        min_key= cur_key;
        min_row_num= cur_row_num;
        bitmap_clear_all(&matching_keys);
        bitmap_set_bit(&matching_keys, min_key->get_keyid());
        bitmap_union(&matching_keys, &matching_outer_cols);
      }
    }

    if (cur_key->next_same())
      queue_insert(&pq, (uchar *) cur_key);

    if (pq.elements == 0)
    {
      /* Check the last row of the last column in PQ for NULL matches. */
      res= test_null_row(min_row_num);
      goto end;
    }
  }

  /* We should never get here - all branches must be handled explicitly above. */
  DBUG_ASSERT(FALSE);

end:
  if (!has_covering_null_columns)
    bitmap_clear_all(&matching_keys);
  queue_remove_all(&pq);
  tmp_table->file->ha_rnd_end();
  return res;
}


subselect_table_scan_engine::subselect_table_scan_engine(
  THD *thd,
  subselect_uniquesubquery_engine *engine_arg,
  TABLE *tmp_table_arg,
  Item_subselect *item_arg,
  select_result_interceptor *result_arg,
  List<Item> *equi_join_conds_arg,
  bool has_covering_null_row_arg,
  bool has_covering_null_columns_arg,
  uint count_columns_with_nulls_arg)
  :subselect_partial_match_engine(thd, engine_arg, tmp_table_arg, item_arg,
                                  result_arg, equi_join_conds_arg,
                                  has_covering_null_row_arg,
                                  has_covering_null_columns_arg,
                                  count_columns_with_nulls_arg)
{}


/*
  TIMOUR:
  This method is based on subselect_uniquesubquery_engine::scan_table().
  Consider refactoring somehow, 80% of the code is the same.

  for each row_i in tmp_table
  {
    count_matches= 0;
    for each row element row_i[j]
    {
      if (outer_ref[j] is NULL || row_i[j] is NULL || outer_ref[j] == row_i[j])
        ++count_matches;
    }
    if (count_matches == outer_ref.elements)
      return TRUE
  }
  return FALSE
*/

bool subselect_table_scan_engine::partial_match()
{
  List_iterator_fast<Item> equality_it(*equi_join_conds);
  Item *cur_eq;
  uint count_matches;
  int error;
  bool res;

  if (unlikely(tmp_table->file->ha_rnd_init_with_error(1)))
  {
    res= FALSE;
    goto end;
  }

  tmp_table->file->extra_opt(HA_EXTRA_CACHE,
                             get_thd()->variables.read_buff_size);
  for (;;)
  {
    error= tmp_table->file->ha_rnd_next(tmp_table->record[0]);
    if (unlikely(error))
    {
      if (error == HA_ERR_END_OF_FILE)
      {
        error= 0;
        break;
      }
      else
      {
        error= report_error(tmp_table, error);
        break;
      }
    }

    equality_it.rewind();
    count_matches= 0;
    while ((cur_eq= equality_it++))
    {
      DBUG_ASSERT(cur_eq->type() == Item::FUNC_ITEM &&
                  ((Item_func*)cur_eq)->functype() == Item_func::EQ_FUNC);
      if (!cur_eq->val_int() && !cur_eq->null_value)
        break;
      ++count_matches;
    }
    if (count_matches == tmp_table->s->fields)
    {
      res= TRUE; /* Found a matching row. */
      goto end;
    }
  }

  res= FALSE;
end:
  tmp_table->file->ha_rnd_end();
  return res;
}


void subselect_table_scan_engine::cleanup()
{
}


subselect_single_column_match_engine::subselect_single_column_match_engine(
  THD *thd,
  subselect_uniquesubquery_engine *engine_arg,
  TABLE *tmp_table_arg,
  Item_subselect *item_arg,
  select_result_interceptor *result_arg,
  List<Item> *equi_join_conds_arg,
  bool has_covering_null_row_arg,
  bool has_covering_null_columns_arg,
  uint count_columns_with_nulls_arg)
  :subselect_partial_match_engine(thd, engine_arg, tmp_table_arg, item_arg,
                                  result_arg, equi_join_conds_arg,
                                  has_covering_null_row_arg,
                                  has_covering_null_columns_arg,
                                  count_columns_with_nulls_arg)
{}


bool subselect_single_column_match_engine::partial_match()
{
  /*
    We get here if:
    - there is only one column in the materialized table;
    - its current value of left_expr is NULL (otherwise we would have hit
         the earlier "index lookup" branch at subselect_partial_match::exec());
    - the materialized table does not have NULL values (for a similar reason);
    - the materialized table is not empty.
    The case when materialization produced no rows (empty table) is handled at
    subselect_hash_sj_engine::exec(), the result of IN predicate is always
    FALSE in that case.
    After all those preconditions met, the result of the partial match is TRUE.
  */
  DBUG_ASSERT(item->get_IN_subquery()->left_expr_has_null() &&
              !has_covering_null_row &&
              tmp_table->file->stats.records > 0);
  return true;
}


void Item_subselect::register_as_with_rec_ref(With_element *with_elem)
{
  with_elem->sq_with_rec_ref.link_in_list(this, &this->next_with_rec_ref);
  with_recursive_reference= true;
}


/*
  Create an execution tracker for the expression cache we're using for this
  subselect; add the tracker to the query plan.
*/

void Item_subselect::init_expr_cache_tracker(THD *thd)
{
  if(!expr_cache)
    return;

  Explain_query *qw= thd->lex->explain;
  DBUG_ASSERT(qw);
  Explain_node *node= qw->get_node(unit->first_select()->select_number);
  if (!node)
    return;
  DBUG_ASSERT(expr_cache->type() == Item::EXPR_CACHE_ITEM);
  node->cache_tracker= ((Item_cache_wrapper *)expr_cache)->init_tracker(qw->mem_root);
}


void Subq_materialization_tracker::report_partial_merge_keys(
    Ordered_key **merge_keys, uint merge_keys_count)
{
  partial_match_array_sizes.resize(merge_keys_count, 0);
  for (uint i= 0; i < merge_keys_count; i++)
    partial_match_array_sizes[i]= merge_keys[i]->get_key_buff_elements();
}


/*
  Check if somewhere inside this subselect we read the table. This means a
  full read "(SELECT ... FROM tbl)", outside reference to tbl.column does not
  count
*/

bool
Item_subselect::subselect_table_finder_processor(void *arg)
{
  subselect_table_finder_param *param= (subselect_table_finder_param *)arg;
  for (SELECT_LEX *sl= unit->first_select(); sl; sl= sl->next_select())
  {
    TABLE_LIST *dup;
    if ((dup= sl->find_table(param->thd, &param->find->db,
                             &param->find->table_name)))
    {
      param->dup= dup;
      return TRUE;
    }
  }
  return FALSE;
};
