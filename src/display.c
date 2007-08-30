/* display.c
 * SRE, Thu May 23 08:18:05 2002 [St. Louis]
 * SVN $Id$
 * 
 * Routines for formatting and displaying parse trees
 * for output.
 * 
 *****************************************************************
 * @LICENSE@
 *****************************************************************  
 */

#include "esl_config.h"
#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>

#include "easel.h"
#include "esl_stack.h"
#include "esl_vectorops.h"

#include "funcs.h"
#include "structs.h"

static int *createMultifurcationOrderChart(CM_t *cm);
static void createFaceCharts(CM_t *cm, int **ret_inface, int **ret_outface);

/* Function:  CreateFancyAli()
 * Incept:    SRE, Thu May 23 13:46:09 2002 [St. Louis]
 *
 * Purpose:   Given a trace (and the model and sequence it corresponds
 *            to), create a pairwise alignment for display; return in a Fancyali_t
 *            structure.
 *
 * Args:      tr    - parsetree for cm aligned to dsq
 *            cm    - model
 *            cons  - consensus information for cm; see CreateCMConsensus()
 *            dsq   - digitized sequence
 *            abc   - alphabet to create alignment with (often cm->abc)
 *            i0    - position of first residue in sq to align (1 for first residue)
 *
 * Returns:   fancy alignment structure.
 *            Caller frees, with FreeFancyAli(ali).
 *
 * Xref:      STL6 p.58
 */
Fancyali_t *
CreateFancyAli(Parsetree_t *tr, CM_t *cm, CMConsensus_t *cons, ESL_DSQ *dsq, const ESL_ALPHABET *abc)
{
  /* Contract check. We allow the caller to specify the alphabet they want the 
   * resulting MSA in, but it has to make sense (see next few lines). */
  if(cm->abc->type == eslRNA)
    { 
      if(abc->type != eslRNA && abc->type != eslDNA)
	esl_fatal("ERROR in CreateFancyAli(), cm alphabet is RNA, but requested output alphabet is neither DNA nor RNA.");
    }
  else if(cm->abc->K != abc->K)
    esl_fatal("ERROR in CreateFancyAli(), cm alphabet size is %d, but requested output alphabet size is %d.", cm->abc->K, abc->K);

  int         status;
  Fancyali_t *ali;              /* alignment structure we're building        */
  ESL_STACK  *pda;              /* pushdown automaton used to traverse trace */
  int         type;		/* type of pda move: PDA_RESIDUE, PDA_STATE  */
  int         v;		/* state index       */
  int         nd;		/* node index        */
  int         ti;		/* position in trace */
  int         qinset, tinset;	/* # consensus nt skipped by an EL, or in an EL */
  int         ninset;		/* max # nt in an EL     */
  int         pos;		/* position in growing ali */
  int         lc, rc;		/* indices for left, right pos in consensus */
  int         symi, symj;
  int         d;
  int         mode;
  int         lannote, rannote; /* chars in annotation line; left, right     */
  int         lstr, rstr;	/* chars in structure line; left, right      */
  int         lcons, rcons;	/* chars in consensus line; left, right      */
  int         lmid, rmid;	/* chars in ali quality line; left, right    */
  int         lseq, rseq;	/* chars in aligned target line; left, right */
  int         do_left, do_right;/* flags to generate left, right             */
  int cpos_l, cpos_r;   	/* positions in consensus (1..clen)          */
  int spos_l, spos_r;		/* positions in dsq (1..L)                   */

  ESL_ALLOC(ali, sizeof(Fancyali_t));
  
  /* Calculate length of the alignment display.
   *   MATP node        : +2
   *   MATL, MATR node  : +1
   *   IL, IR state     : +1
   *   EL:              : 4 + width of length display : "*[nn]*"
   *   anything else    : 0.
   */
  ali->len = 0;
  for (ti = 0; ti < tr->n; ti++)
    {
      v  = tr->state[ti];
      if (v == cm->M) {  /* special case: local exit into EL */
	nd = cm->ndidx[tr->state[ti-1]]; /* calculate node that EL replaced */
	qinset     = cons->rpos[nd] - cons->lpos[nd] + 1;
	tinset     = tr->emitr[ti]  - tr->emitl[ti]  + 1;
	ninset     = ESL_MAX(qinset,tinset);
	ali->len += 4;
	do { ali->len++; ninset/=10; } while (ninset); /* poor man's (int)log_10(ninset)+1 */
	continue;
      } else {
	nd = cm->ndidx[v];
	if      (cm->sttype[v]  == IL_st   || cm->sttype[v]  == IR_st)  
	  ali->len += 1;
	else if (cm->ndtype[nd] == MATL_nd || cm->ndtype[nd] == MATR_nd) 
	  ali->len += 1;
	else if (cm->ndtype[nd] == MATP_nd)                              
	  ali->len += 2;
      }	
    }

  /* Allocate and initialize.
   * Blank the annotation lines (memset calls) - only needed
   * because of the way we deal w/ EL. 
   */
  if (cm->annote != NULL ) 
    ESL_ALLOC(ali->annote, sizeof(char) * (ali->len+1));
  else                     
    ali->annote = NULL;
  ESL_ALLOC(ali->cstr, sizeof(char) * (ali->len+1));
  ESL_ALLOC(ali->cseq, sizeof(char) * (ali->len+1));
  ESL_ALLOC(ali->mid,  sizeof(char) * (ali->len+1));
  ESL_ALLOC(ali->aseq, sizeof(char) * (ali->len+1));
  ESL_ALLOC(ali->scoord, sizeof(int)  * ali->len);
  ESL_ALLOC(ali->ccoord, sizeof(int)  * ali->len);

  if (cm->annote != NULL) memset(ali->annote, ' ', ali->len);
  memset(ali->cstr, ' ', ali->len);
  memset(ali->cseq, ' ', ali->len);
  memset(ali->mid,  ' ', ali->len);
  memset(ali->aseq, ' ', ali->len);
  for (pos = 0; pos < ali->len; pos++) 
    ali->ccoord[pos] = ali->scoord[pos] = 0;

  /* Fill in the lines: traverse the traceback.
   */
  pos = 0;
  pda = esl_stack_ICreate();
  esl_stack_IPush(pda, 0);
  esl_stack_IPush(pda, PDA_STATE);
  while (esl_stack_IPop(pda, &type) != eslEOD)
    {
      if (type == PDA_RESIDUE) {
	if (cm->annote != NULL) { 
	  esl_stack_IPop(pda, &rannote); 
	  ali->annote[pos] = rannote;
	}
	esl_stack_IPop(pda, &rstr); 	  ali->cstr[pos]   = rstr;
	esl_stack_IPop(pda, &rcons);	  ali->cseq[pos]   = rcons;
	esl_stack_IPop(pda, &rmid);	  ali->mid[pos]    = rmid;
	esl_stack_IPop(pda, &rseq);    ali->aseq[pos]   = rseq;
	esl_stack_IPop(pda, &cpos_r);  ali->ccoord[pos] = cpos_r;
	esl_stack_IPop(pda, &spos_r);  ali->scoord[pos] = spos_r;
	pos++;
	continue;
      }
	
      /* Else, we're PDA_STATE - e.g. dealing with a trace node.
       */
      esl_stack_IPop(pda, &ti);
      v = tr->state[ti];

      /* Deal with EL (local ends, state M) as a special case.
       * We get away with only writing into aseq because we've
       * memset() the display strings to blank.
       */
      if (v == cm->M) { 
	int numwidth;		/* number of chars to leave for displaying width numbers */

	nd = 1 + cm->ndidx[tr->state[ti-1]]; /* calculate node that EL replaced */
	qinset     = cons->rpos[nd] - cons->lpos[nd] + 1;
	tinset     = tr->emitr[ti]  - tr->emitl[ti]  + 1;
	ninset     = ESL_MAX(qinset,tinset);
	numwidth = 0; do { numwidth++; ninset/=10; } while (ninset); /* poor man's (int)log_10(ninset)+1 */
	memset(ali->cstr+pos,  '~', numwidth+4);
	sprintf(ali->cseq+pos, "*[%*d]*", numwidth, qinset);
	sprintf(ali->aseq+pos, "*[%*d]*", numwidth, tinset);
	pos += 4 + numwidth;
	continue;
      }

      /* Fetch some info into tmp variables, for "clarity"
       */
      nd = cm->ndidx[v];	  /* what CM node we're in */
      lc   = cons->lpos[nd];	  /* where CM node aligns to in consensus */
      rc   = cons->rpos[nd];
      symi = dsq[tr->emitl[ti]];  /* residue indices that node is aligned to */
      symj = dsq[tr->emitr[ti]];
      d = tr->emitr[ti] - tr->emitl[ti] + 1;
      mode = tr->mode[ti];

      /* Calculate four of the five lines: annote, str, cons, and seq.
       */
      do_left = do_right = FALSE;
      if (cm->sttype[v] == IL_st) {
	do_left = TRUE;
	if (cm->annote != NULL) lannote = '.';
	lstr    = '.';
	lcons   = '.';
	if (mode == 3 || mode == 2) lseq = tolower((int) abc->sym[symi]);
        else                        lseq = '~';
	cpos_l  = 0;
	spos_l  = tr->emitl[ti];
      } else if (cm->sttype[v] == IR_st) {
	do_right = TRUE;
	if (cm->annote != NULL) rannote = '.';
	rstr    = '.';
	rcons   = '.';
	if (mode == 3 || mode == 1) rseq = tolower((int) abc->sym[symj]);
        else                        rseq = '~';
	cpos_r  = 0;
	spos_r  = tr->emitr[ti];
      } else {
	if (cm->ndtype[nd] == MATP_nd || cm->ndtype[nd] == MATL_nd) {
	  do_left = TRUE;
	  if (cm->annote != NULL) lannote = cm->annote[lc];
	  lstr   = cons->cstr[lc];
	  lcons  = cons->cseq[lc];
	  cpos_l = lc+1;
	  if (cm->sttype[v] == MP_st || cm->sttype[v] == ML_st) {
	    if      (mode == 3)         lseq = abc->sym[symi];
            else if (mode == 2 && d>0 ) lseq = abc->sym[symi];
            else                        lseq = '~';
	    spos_l = tr->emitl[ti];
	  } else {
	    if (mode == 3 || mode == 2) lseq = '-';
            else                        lseq = '~';
	    spos_l = 0;
	  }
	}
	if (cm->ndtype[nd] == MATP_nd || cm->ndtype[nd] == MATR_nd) {
	  do_right = TRUE;
	  if (cm->annote != NULL) rannote = cm->annote[rc];
	  rstr   = cons->cstr[rc];
	  rcons  = cons->cseq[rc];
	  cpos_r = rc+1;
	  if (cm->sttype[v] == MP_st || cm->sttype[v] == MR_st) {
	    if      (mode == 3)         rseq = abc->sym[symj];
            else if (mode == 1 && d>0 ) rseq = abc->sym[symj];
            else                        rseq = '~';
	    spos_r = tr->emitr[ti];
	  } else {
	    if (mode == 3 || mode == 1) rseq = '-';
            else                        rseq = '~';
	    spos_r = 0;
	  }
	}
      }

      /* Use emission p and score to set lmid, rmid line for emitting states.
       */
      lmid = rmid = ' ';
      if (cm->sttype[v] == MP_st) {
	if (lseq == toupper(lcons) && rseq == toupper(rcons))
	  {
	    lmid = lseq;
	    rmid = rseq;
	  }
        else if (mode != 3)
          ;
	else if (IsCompensatory(cm->abc, cm->e[v], symi, symj)) 
	  lmid = rmid = ':';
	else if (DegeneratePairScore(cm->abc, cm->esc[v], symi, symj) >= 0)
	  lmid = rmid = '+';
      } else if (cm->sttype[v] == ML_st || cm->sttype[v] == IL_st) {
	if (lseq == toupper(lcons)) 
	  lmid = lseq;
        else if ( (mode != 3) && (mode != 2) )
          ;
	else if(esl_abc_FAvgScore(cm->abc, symi, cm->esc[v]) > 0)
	  lmid = '+';
      } else if (cm->sttype[v] == MR_st || cm->sttype[v] == IR_st) {
	if (rseq == toupper(rcons)) 
	  rmid = rseq;
        else if ( (mode != 3) && (mode != 1) )
          ;
	else if(esl_abc_FAvgScore(cm->abc, symj, cm->esc[v]) > 0)
	  rmid = '+';
      }

      /* If we're storing a residue leftwise - just do it.
       * If rightwise - push it onto stack.
       */
      if (do_left) {
	if (cm->annote != NULL) ali->annote[pos] = lannote;
	ali->cstr[pos]   = lstr;
	ali->cseq[pos]   = lcons;
	ali->mid[pos]    = lmid;
	ali->aseq[pos]   = lseq;
	ali->ccoord[pos] = cpos_l;
	ali->scoord[pos] = spos_l;
	pos++;
      }
      if (do_right) {
	esl_stack_IPush(pda, spos_r);
	esl_stack_IPush(pda, cpos_r);
	esl_stack_IPush(pda, (int) rseq);
	esl_stack_IPush(pda, (int) rmid);
	esl_stack_IPush(pda, (int) rcons);
	esl_stack_IPush(pda, (int) rstr);
	if (cm->annote != NULL) esl_stack_IPush(pda, (int) rannote);
	esl_stack_IPush(pda, PDA_RESIDUE);
      }

      /* Push the child trace nodes onto the PDA;
       * right first, so it pops last.
       */
      if (tr->nxtr[ti] != -1) {
	esl_stack_IPush(pda, tr->nxtr[ti]);
	esl_stack_IPush(pda, PDA_STATE);
      }
      if (tr->nxtl[ti] != -1) {
	esl_stack_IPush(pda, tr->nxtl[ti]);
	esl_stack_IPush(pda, PDA_STATE);
      }
    } /* end loop over the PDA; PDA now empty */
	 
  if (cm->annote != NULL) ali->annote[ali->len] = '\0';
  ali->cstr[ali->len] = '\0';
  ali->cseq[ali->len] = '\0';
  ali->mid[ali->len]  = '\0';
  ali->aseq[ali->len] = '\0';

  /* Laboriously determine the maximum bounds.
   */
  ali->sqfrom = 0;
  for (pos = 0; pos < ali->len; pos++)
    if (ali->scoord[pos] != 0) {
      ali->sqfrom = ali->scoord[pos];
      break;
    }
  ali->sqto = 0;
  for (pos = 0; pos < ali->len; pos++)
    if (ali->scoord[pos] != 0) ali->sqto = ali->scoord[pos];
  ali->cfrom = 0; 
  for (pos = 0; pos < ali->len; pos++)
    if (ali->ccoord[pos] != 0) {
      ali->cfrom = ali->ccoord[pos];
      break;
    }
  ali->cto = 0;
  for (pos = 0; pos < ali->len; pos++)
    if (ali->ccoord[pos] != 0) ali->cto = ali->ccoord[pos];

  esl_stack_Destroy(pda);
  return ali;

 ERROR:
  esl_fatal("Memory allocation error.\n");
  return NULL; /* not reached */
}

/* Function: PrintFancyAli()
 * Date:     SRE, Thu Jun 13 02:23:18 2002 [Aula Magna, Stockholm]
 *
 * Purpose:  Write a CM/sequence alignment to a stream, from a
 *           Fancyali_t structure. Line length currently hardcoded
 *           but this could be changed. Modeled on HMMER's 
 *           eponymous function.
 *
 * Args:     fp  - where to print it (stdout or open FILE)
 *           ali - alignment structure to print.      
 *           offset- number of residues to add to target seq index,
 *                   to ease MPI search, all target hits start at posn 1
 *           in_revcomp- TRUE if hit we're printing an alignment for a
 *                       cmsearch hit on reverse complement strand.
 * Returns:  (void)
 */
void
PrintFancyAli(FILE *fp, Fancyali_t *ali, int offset, int in_revcomp)
{
  int   status;
  char *buf;
  int   pos;
  int   linelength;
  int   ci,  cj;		/* positions in CM consensus 1..clen */
  int   sqi, sqj;		/* positions in target seq 1..L      */
  int   i;
  int   i2print, j2print; /* i,j indices we'll print, used to deal
				 * with case of reverse complement */
  linelength = 60;
  ESL_ALLOC(buf, sizeof(char) * (linelength + 1));
  buf[linelength] = '\0';
  for (pos = 0; pos < ali->len; pos += linelength)
    {
      /* Laboriously determine our coord bounds on dsq
       * and consensus line for this alignment section.
       */
      sqi = 0;
      for (i = pos; ali->aseq[i] != '\0' && i < pos + linelength; i++) {
	if (ali->scoord[i] != 0) {
	  sqi = ali->scoord[i];
	  break;
	}
      }
      sqj = 0;
      for (i = pos; ali->aseq[i] != '\0' && i < pos + linelength; i++) {
	if (ali->scoord[i] != 0) sqj = ali->scoord[i];
      }
      ci = 0; 
      for (i = pos; ali->aseq[i] != '\0' && i < pos + linelength; i++) {
	if (ali->ccoord[i] != 0) {
	  ci = ali->ccoord[i];
	  break;
	}
      }
      cj = 0;
      for (i = pos; ali->aseq[i] != '\0' && i < pos + linelength; i++) {
	if (ali->ccoord[i] != 0) cj = ali->ccoord[i];
      }

      /* Formats and print the alignment section.
       */
      if (ali->annote != NULL) {
	strncpy(buf, ali->annote+pos, linelength);
	fprintf(fp, "  %8s %s\n", " ", buf);
      }
      if (ali->cstr != NULL) {
	strncpy(buf, ali->cstr+pos, linelength);  
	fprintf(fp, "  %8s %s\n", " ", buf);
      }
      if (ali->cseq != NULL) {
	strncpy(buf, ali->cseq+pos, linelength);  
	if (ci && cj)
	  fprintf(fp, "  %8d %s %-8d\n", ci, buf, cj);
	else
	  fprintf(fp, "  %8s %s %-8s\n", "-", buf, "-");
      }
      if (ali->mid != NULL) {
	strncpy(buf, ali->mid+pos,  linelength);  
	fprintf(fp, "  %8s %s\n", " ", buf);
      }
      if (ali->aseq != NULL) {
	strncpy(buf, ali->aseq+pos, linelength);  
	if (sqj && sqi) 
	  {
	    if(in_revcomp) 
	      {
		i2print = offset - (sqi-1)    + 1;
		j2print = i2print - (sqj-sqi);
	      }
	    else
	      {
		i2print = sqi + offset;
		j2print = sqj + offset;
	      }
	    fprintf(fp, "  %8d %s %-8d\n", i2print, buf, j2print);
	  }
	else
	  fprintf(fp, "  %8s %s %-8s\n", "-", buf, "-");
      }
      fprintf(fp, "\n");
    }
  free(buf);
  fflush(fp);
  return;

 ERROR:
  esl_fatal("Memory allocation error.\n");
}


/* Function:  FreeFancyAli()
 * Incept:    SRE, Fri May 24 15:37:30 2002 [St. Louis]
 */
void
FreeFancyAli(Fancyali_t *ali)
{
  if (ali->annote != NULL) free(ali->annote);
  if (ali->cstr   != NULL) free(ali->cstr);
  if (ali->cseq   != NULL) free(ali->cseq);
  if (ali->mid    != NULL) free(ali->mid);
  if (ali->aseq   != NULL) free(ali->aseq);
  if (ali->ccoord != NULL) free(ali->ccoord);
  if (ali->scoord != NULL) free(ali->scoord);
  free(ali);
}


/* Function:  CreateCMConsensus()
 * Incept:    SRE, Thu May 23 10:39:39 2002 [St. Louis]
 *
 * Purpose:   Create displayable strings for consensus sequence
 *            and consensus structure; also create map of 
 *            nodes -> right and left consensus positions.
 *            
 *            Consensus sequence shows maximum scoring residue(s)
 *            for each emitting node. If score < pthresh (for pairs)
 *            or < sthresh (for singlets), the residue is shown
 *            in lower case. (That is, "strong" consensus residues
 *            are in upper case.)
 *            
 *            Consensus structure annotates
 *            base pairs according to "multifurcation order" (how
 *            many multifurcation loops are beneath this pair).
 *               terminal stems:  <>
 *               order 1:         ()
 *               order 2:         []
 *               order >2:        {}
 *            Singlets are annotated : if external, _ if hairpin,
 *            - if bulge or interior loop, and , for multifurcation loop.
 *               
 *            Example:
 *                ::(((,,<<<__>>>,<<<__>>->,,)))::
 *                AAGGGAACCCTTGGGTGGGTTCCACAACCCAA   
 *
 * Args:      cm         - the model
 *            abc        - alphabet to create con->cseq with (often cm->abc)
 *            pthresh    - bit score threshold for base pairs to be lowercased
 *            sthresh    - bit score threshold for singlets to be lowercased
 *            
 * Returns:   <eslOK> on success, <eslEMEM> on memory error.
8             CMConsensus_t structure in *ret_cons.
 *            Caller frees w/ FreeCMConsensus().
 *
 * Xref:      STL6 p.58.
 */
int
CreateCMConsensus(CM_t *cm, const ESL_ALPHABET *abc, float pthresh, float sthresh, CMConsensus_t **ret_cons)
{
  /* Contract check. We allow the caller to specify the alphabet they want the 
   * resulting MSA in, but it has to make sense (see next few lines). */
  if(cm->abc->type == eslRNA)
    { 
      if(abc->type != eslRNA && abc->type != eslDNA)
	esl_fatal("ERROR in CreateFancyAli(), cm alphabet is RNA, but requested output alphabet is neither DNA nor RNA.");
    }
  else if(cm->abc->K != abc->K)
    esl_fatal("ERROR in CreateFancyAli(), cm alphabet size is %d, but requested output alphabet size is %d.", cm->abc->K, abc->K);

  int       status;
  CMConsensus_t *con;           /* growing consensus info */
  char     *cseq;               /* growing consensus sequence display string   */
  char     *cstr;               /* growing consensus structure display string  */
  int      *ct;			/* growing ct Zuker pairing partnet string     */
  int      *lpos, *rpos;        /* maps node->consensus position, [0..nodes-1] */
  int       cpos;		/* current position in cseq, cstr              */
  int       nalloc;		/* current allocated length of cseq, cstr      */
  ESL_STACK *pda;               /* pushdown automaton used to traverse model   */
  int      *multiorder;         /* "height" of each node (multifurcation order), [0..nodes-1] */
  int      *inface;             /* face count for each node, inside */
  int      *outface;            /* face count for each node, outside */
  int       v, nd;
  int       type;
  char      lchar, rchar;
  char      lstruc, rstruc;
  int       x;
  int       pairpartner;	/* coord of left pairing partner of a right base */
  void     *tmp;                /* for ESL_RALLOC */

  ESL_ALLOC(lpos, sizeof(int) * cm->nodes);
  ESL_ALLOC(rpos, sizeof(int) * cm->nodes);
  ESL_ALLOC(cseq, sizeof(char) * 100);
  ESL_ALLOC(cstr, sizeof(char) * 100);
  ESL_ALLOC(ct,   sizeof(int)  * 100);
  nalloc  = 100;
  cpos    = 0;

  for (nd = 0; nd < cm->nodes; nd++) 
    lpos[nd] = rpos[nd] = -1;

  multiorder = createMultifurcationOrderChart(cm);
  createFaceCharts(cm, &inface, &outface);

  pda = esl_stack_ICreate();
  esl_stack_IPush(pda, 0);
  esl_stack_IPush(pda, PDA_STATE);
  while (esl_stack_IPop(pda, &type) != eslEOD) {
    if (type == PDA_RESIDUE) 
      {
	esl_stack_IPop(pda, &x); rchar  = (char) x;
	esl_stack_IPop(pda, &x); rstruc = (char) x;
	esl_stack_IPop(pda, &pairpartner); 
	esl_stack_IPop(pda, &nd);
	rpos[nd]   = cpos;
	cseq[cpos] = rchar;
	cstr[cpos] = rstruc;
	ct[cpos]   = pairpartner;
	if (pairpartner != -1) ct[pairpartner] = cpos;
	cpos++;
      }
    else if (type == PDA_MARKER) 
      {
	esl_stack_IPop(pda, &nd);
	rpos[nd]   = cpos;
      }
    else if (type == PDA_STATE) 
      {
	esl_stack_IPop(pda, &v);
	nd    = cm->ndidx[v];
	lchar = rchar = lstruc = rstruc = 0;

	/* Determine what we emit: 
	 * MATP, MATL, MATR consensus states only.
	 */
	if (cm->stid[v] == MATP_MP) 
	  {
	    x = esl_vec_FArgMax(cm->esc[v], abc->K*abc->K);
	    lchar = abc->sym[x / abc->K];
	    rchar = abc->sym[x % abc->K];
	    if (cm->esc[v][x] < pthresh) {
	      lchar = tolower((int) lchar);
	      rchar = tolower((int) rchar);
	    }
	    switch (multiorder[nd]) {
	    case 0:  lstruc = '<'; rstruc = '>'; break;
	    case 1:  lstruc = '('; rstruc = ')'; break;
	    case 2:  lstruc = '['; rstruc = ']'; break;
	    default: lstruc = '{'; rstruc = '}'; break;
	    }
	} else if (cm->stid[v] == MATL_ML) {
	  x = esl_vec_FArgMax(cm->esc[v], cm->abc->K);
	  lchar = abc->sym[x];
	  if (cm->esc[v][x] < sthresh) lchar = tolower((int) lchar);
	  if      (outface[nd] == 0)                    lstruc = ':'; /* external ss */
	  else if (inface[nd] == 0 && outface[nd] == 1) lstruc = '_'; /* hairpin loop */
	  else if (inface[nd] == 1 && outface[nd] == 1) lstruc = '-'; /* bulge/interior */
	  else                                          lstruc = ','; /* multiloop */
	  rstruc = ' ';
	} else if (cm->stid[v] == MATR_MR) {
	  x = esl_vec_FArgMax(cm->esc[v], cm->abc->K);
	  rchar = abc->sym[x];
	  if (cm->esc[v][x] < sthresh) rchar = tolower((int) rchar);
	  if      (outface[nd] == 0)                    rstruc = ':'; /* external ss */
	  else if (inface[nd] == 0 && outface[nd] == 1) rstruc = '?'; /* doesn't happen */
	  else if (inface[nd] == 1 && outface[nd] == 1) rstruc = '-'; /* bulge/interior */
	  else                                          rstruc = ','; /* multiloop */
	  lstruc = ' ';
	}

	/* Emit. A left base, we can do now; 
	 * a right base, we defer onto PDA.
	 */
	lpos[nd]   = cpos;	/* we always set lpos, rpos even for nonemitters */
	if (lchar) {
	  cseq[cpos] = lchar;
	  cstr[cpos] = lstruc;
	  ct[cpos]   = -1;	/* will be overwritten, if needed, when right guy is processed */
	  cpos++;
	}
	if (rchar) {
	  esl_stack_IPush(pda, nd);
	  if (lchar) esl_stack_IPush(pda, cpos-1);
	  else       esl_stack_IPush(pda, -1);
	  esl_stack_IPush(pda, rstruc);
	  esl_stack_IPush(pda, rchar);
	  esl_stack_IPush(pda, PDA_RESIDUE);
	} else {
	  esl_stack_IPush(pda, nd);
	  esl_stack_IPush(pda, PDA_MARKER);
	}

	/* Transit - to consensus states only.
	 * The obfuscated idiom finds the index of the next consensus
	 * state without making assumptions about numbering or connectivity.
	 */
	if (cm->sttype[v] == B_st) {
	  esl_stack_IPush(pda, cm->cnum[v]);     /* right S  */
	  esl_stack_IPush(pda, PDA_STATE);
	  esl_stack_IPush(pda, cm->cfirst[v]);   /* left S */
	  esl_stack_IPush(pda, PDA_STATE);
	} else if (cm->sttype[v] != E_st) {
	  v = cm->nodemap[cm->ndidx[cm->cfirst[v] + cm->cnum[v] - 1]];
	  esl_stack_IPush(pda, v);
	  esl_stack_IPush(pda, PDA_STATE);
	}
      } /*end PDA_STATE block*/

    if (cpos == nalloc) {
      nalloc += 100;
      ESL_RALLOC(cseq, tmp, sizeof(char) * nalloc);
      ESL_RALLOC(cstr, tmp, sizeof(char) * nalloc);
      ESL_RALLOC(ct,   tmp, sizeof(int)  * nalloc);
    }
  }/* PDA now empty... done generating cseq, cstr, and node->consensus residue map */
  cseq[cpos] = '\0';
  cstr[cpos] = '\0';

  esl_stack_Destroy(pda);
  free(multiorder);
  free(inface);
  free(outface);

  ESL_ALLOC(con, sizeof(CMConsensus_t));
  con->cseq = cseq;
  con->cstr = cstr;
  con->ct   = ct;
  con->lpos = lpos;
  con->rpos = rpos;
  con->clen = cpos;
  *ret_cons = con;
  return eslOK;

 ERROR:
  return status;
}

void
FreeCMConsensus(CMConsensus_t *con)
{
  if (con->cseq != NULL) free(con->cseq);
  if (con->cstr != NULL) free(con->cstr);
  if (con->ct   != NULL) free(con->ct);
  if (con->lpos != NULL) free(con->lpos);
  if (con->rpos != NULL) free(con->rpos);
  free(con);
}

/* Function:  createMultifurcationOrderChart()
 * Incept:    SRE, Thu May 23 09:48:33 2002 [St. Louis] 
 *
 * Purpose:   Calculates the degree of multifurcation beneath
 *            the master subtree rooted at every node n.
 *            Returns [0..nodes-1] array of these values.
 *
 *            Terminal stems have value 0. All nodes n starting with
 *            the BEG node for a terminal stem have height[n] = 0.
 *            
 *            A stem "above" a multifurcation into all terminal stems
 *            has value 1; all nodes n starting with BEG and ending
 *            with BIF have height[n] = 1.
 * 
 *            And so on, for "higher order" (deeper) multifurcations.
 * 
 *            Used for figuring out what characters we'll display a
 *            consensus pair with.
 *            
 *            THIS FUNCTION IS BUGGY (Sat Jun  1 12:24:23 2002)
 *            
 * Args:      cm - the model.
 *
 * Returns:   [0..cm->nodes-1] array of multifurcation orders, for each node.
 *            This array is allocated here; caller free's w/ free().
 *
 * xref:     STL6 p.58.
 */
static int *
createMultifurcationOrderChart(CM_t *cm)
{
  int status;
  int  v, nd, left, right;
  int *height;
  int *seg_has_pairs;

  ESL_ALLOC(height,        sizeof(int) * cm->nodes);
  ESL_ALLOC(seg_has_pairs, sizeof(int) * cm->nodes);
  for (nd = cm->nodes-1; nd >= 0; nd--)
    {
      v = cm->nodemap[nd];

      if       (cm->stid[v] == MATP_MP) seg_has_pairs[nd] = TRUE;
      else if  (cm->stid[v] == END_E)   seg_has_pairs[nd] = FALSE;
      else if  (cm->stid[v] == BIF_B)   seg_has_pairs[nd] = FALSE;
      else                              seg_has_pairs[nd] = seg_has_pairs[nd+1];

      if (cm->stid[v] == END_E) 
	height[nd]        = 0;
      else if (cm->stid[v] == BIF_B) 
	{
	  left  = cm->ndidx[cm->cfirst[v]]; 
	  right = cm->ndidx[cm->cnum[v]];
	  height[nd] = ESL_MAX(height[left] + seg_has_pairs[left],
			       height[right] + seg_has_pairs[right]);
	}
      else
	height[nd] = height[nd+1]; 
    }
  free(seg_has_pairs);
  return height;

 ERROR:
  esl_fatal("Memory allocation error.\n");
  return 0; /* never reached */
}	
	
     
/* Function:  createFaceCharts()
 * Incept:    SRE, Thu May 23 12:40:04 2002 [St. Louis]
 *
 * Purpose:   Calculate "inface" and "outface" for each node
 *            in the consensus (master) structure of the CM.
 *            These can be used to label nodes:
 *                                inface       outface    
 *                             ------------   ----------
 *             external ss         any           0                   
 *             hairpin loop         0            1
 *             bulge/interior       1            1
 *             multifurc           >1            1  
 *             multifurc            1           >1   
 *             doesn't happen       0           >1
 *             
 *             hairpin closing bp   0            1
 *             extern closing bp    1            0
 *             stem bp              1            1
 *             multifurc close bp  >1            1
 *             multifurc close bp   1           >1
 *             doesn't happen       0           >1
 *
 * Args:       cm          - the model
 *             ret_inface  - RETURN: inface[0..nodes-1]
 *             ret_outface - RETURN: outface[0..nodes-1]         
 *
 * Returns:    inface, outface; 
 *             they're alloc'ed here. Caller free's with free()
 *
 * Xref:       STL6 p.58
 */
static void
createFaceCharts(CM_t *cm, int **ret_inface, int **ret_outface)
{
  int  status;
  int *inface;
  int *outface;
  int  nd, left, right, parent;
  int  v,w,y;

  ESL_ALLOC(inface,  sizeof(int) * cm->nodes);
  ESL_ALLOC(outface, sizeof(int) * cm->nodes);

  /* inface - the number of faces below us in descendant
   *          subtrees. if 0, we're either external, or
   *          a closing basepair, or we're in a hairpin loop. 
   *          inface is exclusive of current pair - so we
   *          can easily detect closing base pairs.
   */
  for (nd = cm->nodes-1; nd >= 0; nd--)
    {
      v = cm->nodemap[nd];
      if      (cm->ndtype[nd] == END_nd) inface[nd] = 0;
      else if (cm->ndtype[nd] == BIF_nd) {
	left  = cm->ndidx[cm->cfirst[v]];
	right = cm->ndidx[cm->cnum[v]];
	inface[nd] = inface[left] + inface[right];
      } else {
	if (cm->ndtype[nd+1] == MATP_nd) inface[nd] = 1;
	else                             inface[nd] = inface[nd+1];
      }
    }

  /* outface - the number of faces above us in the tree
   *           excluding our subtree. if 0, we're external.
   *           Like inface, outface is exclusive of current
   *           pair.
   */
  for (nd = 0; nd < cm->nodes; nd++)
    {
      v = cm->nodemap[nd];
      if      (cm->ndtype[nd] == ROOT_nd) outface[nd] = 0;
      else if (cm->ndtype[nd] == BEGL_nd) 
	{
	  parent = cm->ndidx[cm->plast[v]];
	  y      = cm->nodemap[parent];
	  right  = cm->ndidx[cm->cnum[y]];
	  outface[nd] = outface[parent] + inface[right];
	}
      else if (cm->ndtype[nd] == BEGR_nd)
	{
	  parent = cm->ndidx[cm->plast[v]];
	  w      = cm->nodemap[parent];
	  left   = cm->ndidx[cm->cfirst[y]];
	  outface[nd] = outface[parent] + inface[left];
	}
      else 
	{
	  parent = nd-1;
	  if (cm->ndtype[parent] == MATP_nd) outface[nd] = 1;
	  else                               outface[nd] = outface[parent];
	}
    }
  
  *ret_inface  = inface;
  *ret_outface = outface;
  return;

 ERROR:
  esl_fatal("Memory allocation error.\n");
}
	


/* Function: MainBanner()
 * Date:     SRE, Fri Sep 26 11:29:02 2003 [AA 886, from Salk Institute]
 *
 * Purpose:  Print a package version and copyright banner.
 *           Used by all the main()'s that use the squid library.
 *           
 *    Expects to be able to pick up defined preprocessor variables:
 *    variable          example
 *    --------           --------------  
 *    PACKAGE_NAME      "Infernal"
 *    PACKAGE_VERSION   "0.42"
 *    PACKAGE_DATE      "Sept 2003"
 *    PACKAGE_COPYRIGHT "Copyright (C) 2001-2003 Washington University School of Medicine"
 *    PACKAGE_LICENSE   "Freely distributed under the GNU General Public License (GPL)."
 *    This gives us a general mechanism to update release information
 *    without changing multiple points in the code.
 * 
 * Args:     fp     - where to print it
 *           banner - one-line program description, e.g.:
 *                    "foobar - make bars from foo with elan" 
 * Returns:  (void)
 */
void
MainBanner(FILE *fp, char *banner)
{
  fprintf(fp, "%s\n", banner);
  fprintf(fp, "%s %s (%s)\n", PACKAGE_NAME, PACKAGE_VERSION, PACKAGE_DATE);
  fprintf(fp, "%s\n", PACKAGE_COPYRIGHT);
  fprintf(fp, "%s\n", PACKAGE_LICENSE);
  fprintf(fp, "- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -\n");
}


/* Function: IsCompensatory()
 * Date:     SRE, Sun Jun  2 10:16:59 2002 [Madison]
 *
 * Purpose:  Returns TRUE if log[pij/(pi*pj)] is >= 0,
 *           where pij is the probability of a base pair,
 *           pi and pj are the marginal probabilities
 *           for the symbols at i and j.
 *           
 *           Currently returns FALSE if symi or symj
 *           are ambiguous IUPAC nucs. Could do a more
 *           sophisticated marginalization - prob not
 *           worth it right now.                                 
 *           
 * Args:     pij  - joint emission probability vector [0..15]
 *                  indexed symi*4 + symj.
 *           symi - symbol index at i [0..3], equiv to [a..u]
 *           symj - symbol index at j [0..3], equiv to [a..u]
 *
 * Returns:  TRUE or FALSE.
 */
int
IsCompensatory(const ESL_ALPHABET *abc, float *pij, int symi, int symj)
{
  int   x;
  float pi, pj;

  if (symi >= abc->K || symj >= abc->K) 
    return FALSE;

  pi = pj = 0.;
  for (x = 0; x < abc->K; x++) 
    {
      pi += pij[symi*abc->K + x];
      pj += pij[x*abc->K    + symi];
    }
  if (log(pij[symi*abc->K+symj]) - log(pi) - log(pj) >= 0)
    return TRUE;
  else 
    return FALSE;
}
