/*
 * Host tests for the VDM / alternate-mode engine, under ASan/UBSan.
 *
 * app_vdm_target.c is compiled here too, so the real request path - validation
 * followed by the ST PE call - is exercised against stubs that record what was
 * passed.  That proves argument marshalling, not just the state machine.
 */
#include <stdio.h>
#include <string.h>
#include "usbpd_def.h"
#include "app_vdm.h"

/* Stubs for the five ST PE entry points. */
static struct {
    int calls; int last_port; int last_sop; int last_svid; int last_index;
    int force_fail;
} s_id, s_svid, s_mode, s_enter, s_exit;

#define STUB_BODY(rec, n) \
    do { (rec).calls++; (rec).last_port = (int)PortNum; \
         (rec).last_sop = (int)SOPType; \
         return (rec).force_fail ? 1u : 0u; } while (0)

USBPD_StatusTypeDef USBPD_PE_SVDM_RequestIdentity(uint8_t PortNum,
                                                  USBPD_SOPType_TypeDef SOPType)
{ (void)SOPType; STUB_BODY(s_id, 1); }

USBPD_StatusTypeDef USBPD_PE_SVDM_RequestSVID(uint8_t PortNum,
                                              USBPD_SOPType_TypeDef SOPType)
{ (void)SOPType; STUB_BODY(s_svid, 2); }

USBPD_StatusTypeDef USBPD_PE_SVDM_RequestMode(uint8_t PortNum,
                                              USBPD_SOPType_TypeDef SOPType,
                                              uint16_t SVID)
{ (void)SOPType; s_mode.last_svid = (int)SVID; STUB_BODY(s_mode, 3); }

USBPD_StatusTypeDef USBPD_PE_SVDM_RequestModeEnter(uint8_t PortNum,
                                                   USBPD_SOPType_TypeDef SOPType,
                                                   uint16_t SVID, uint8_t Index)
{ (void)SOPType; s_enter.last_svid = (int)SVID; s_enter.last_index = (int)Index;
  STUB_BODY(s_enter, 4); }

USBPD_StatusTypeDef USBPD_PE_SVDM_RequestModeExit(uint8_t PortNum,
                                                  USBPD_SOPType_TypeDef SOPType,
                                                  uint16_t SVID, uint8_t Index)
{ (void)SOPType; s_exit.last_svid = (int)SVID; s_exit.last_index = (int)Index;
  STUB_BODY(s_exit, 5); }

static void reset_all(void)
{
    memset(&s_id, 0, sizeof(s_id));
    memset(&s_svid, 0, sizeof(s_svid));
    memset(&s_mode, 0, sizeof(s_mode));
    memset(&s_enter, 0, sizeof(s_enter));
    memset(&s_exit, 0, sizeof(s_exit));
    APP_VDM_Init();
}

static int s_fail;
static void eq(long got, long want, const char *what)
{
    if (got != want) {
        s_fail++;
        printf("  FAIL %s: got %ld want %ld\n", what, got, want);
    }
}

static void test_validation(void)
{
    printf("test_validation\n");
    reset_all();

    /* Well-formed requests are accepted and reach the PE. */
    eq(APP_VDM_RequestIdentity(0u, APP_VDM_SOP1), APP_VDM_CALL_OK,
       "identity accepted");
    eq(s_id.calls, 1, "identity reached PE");
    eq(s_id.last_sop, APP_VDM_SOP1, "identity SOP'");

    reset_all();
    eq(APP_VDM_RequestSVID(0u, APP_VDM_SOP), APP_VDM_CALL_OK, "svids accepted");
    eq(s_svid.calls, 1, "svids reached PE");

    /* Bad port. */
    reset_all();
    eq(APP_VDM_RequestIdentity(1u, APP_VDM_SOP1), APP_VDM_CALL_REJECTED,
       "port 1 rejected");
    eq(s_id.calls, 0, "rejected before PE");

    /* Bad SOP type. */
    reset_all();
    eq(APP_VDM_RequestIdentity(0u, 9u), APP_VDM_CALL_REJECTED, "sop 9 rejected");

    /* SOP'' is legal. */
    reset_all();
    eq(APP_VDM_RequestIdentity(0u, APP_VDM_SOP2), APP_VDM_CALL_OK,
       "SOP'' accepted");

    /* SVID is required for mode discovery and enter/exit. */
    reset_all();
    eq(APP_VDM_RequestMode(0u, APP_VDM_SOP, 0u), APP_VDM_CALL_REJECTED,
       "modes svid 0 rejected");
    eq(APP_VDM_ModeEnter(0u, APP_VDM_SOP, 0u, 1u), APP_VDM_CALL_REJECTED,
       "enter svid 0 rejected");

    /* Mode index range: enter needs 1..6. */
    reset_all();
    eq(APP_VDM_ModeEnter(0u, APP_VDM_SOP, 0xFF01u, 0u), APP_VDM_CALL_REJECTED,
       "enter index 0 rejected");
    reset_all();
    eq(APP_VDM_ModeEnter(0u, APP_VDM_SOP, 0xFF01u, 7u), APP_VDM_CALL_REJECTED,
       "enter index 7 rejected");
    reset_all();
    eq(APP_VDM_ModeEnter(0u, APP_VDM_SOP, 0xFF01u, 1u), APP_VDM_CALL_OK,
       "enter index 1 accepted");
    eq(s_enter.last_svid, 0xFF01, "enter svid marshalled");
    eq(s_enter.last_index, 1, "enter index marshalled");

    /* Exit allows index 0 ("all modes"). */
    reset_all();
    eq(APP_VDM_ModeExit(0u, APP_VDM_SOP, 0xFF01u, 0u), APP_VDM_CALL_OK,
       "exit index 0 accepted");

    /* Only one transaction outstanding: a second request is refused until the
     * first is answered, so the user gets a clear answer instead of a queue. */
    reset_all();
    eq(APP_VDM_ModeEnter(0u, APP_VDM_SOP, 0xFF01u, 1u), APP_VDM_CALL_OK,
       "first enter");
    eq(APP_VDM_ModeEnter(0u, APP_VDM_SOP, 0xFF01u, 2u), APP_VDM_CALL_REJECTED,
       "second enter refused while pending");
    eq(s_enter.calls, 1, "PE called once");
}

static void test_pe_rejection(void)
{
    printf("test_pe_rejection\n");
    reset_all();
    s_enter.force_fail = 1;

    eq(APP_VDM_ModeEnter(0u, APP_VDM_SOP, 0xFF01u, 1u), APP_VDM_CALL_REJECTED,
       "PE refusal reported");
    eq(APP_VDM_Get()->last_rejected, 1, "rejection recorded");
    eq(APP_VDM_Get()->pending, 0, "latch cleared after refusal");
    eq(APP_VDM_Get()->n_rejected, 1, "rejection counted");
    eq(APP_VDM_Get()->n_enter_req, 1, "attempt counted");

    /* Clearing the latch means the next request goes through. */
    eq(APP_VDM_ModeEnter(0u, APP_VDM_SOP, 0xFF01u, 1u), APP_VDM_CALL_REJECTED,
       "still refused while PE is failing");
    s_enter.force_fail = 0;
    eq(APP_VDM_ModeEnter(0u, APP_VDM_SOP, 0xFF01u, 1u), APP_VDM_CALL_OK,
       "accepted once PE recovers");
}

static void test_status_classifier(void)
{
    printf("test_status_classifier\n");

    /* Only an ACK'd Enter enters the mode; only an ACK'd Exit leaves it. */
    eq(APP_VDM_ApplyStatus(0u, 1u, APP_VDM_STAT_ACK), 1u, "ACK enter -> in");
    eq(APP_VDM_ApplyStatus(1u, 0u, APP_VDM_STAT_ACK), 0u, "ACK exit -> out");
    eq(APP_VDM_ApplyStatus(0u, 1u, APP_VDM_STAT_NAK), 0u, "NAK enter -> stay out");
    eq(APP_VDM_ApplyStatus(1u, 1u, APP_VDM_STAT_NAK), 1u, "NAK enter -> stay in");
    eq(APP_VDM_ApplyStatus(1u, 0u, APP_VDM_STAT_BUSY), 1u, "BUSY exit -> stay in");
    eq(APP_VDM_ApplyStatus(0u, 0u, APP_VDM_STAT_ACK), 0u, "ACK exit -> out");
    eq(APP_VDM_ApplyStatus(1u, 0u, 0u), 1u, "unknown status -> unchanged");
}

static void test_response_path(void)
{
    printf("test_response_path\n");
    reset_all();

    eq(APP_VDM_Get()->in_alt_mode, 0, "starts out of alt mode");

    /* A NAK'd enter must not report the mode as entered. */
    APP_VDM_ModeEnter(0u, APP_VDM_SOP, 0xFF01u, 1u);
    APP_VDM_OnModeEnter(0u, APP_VDM_SOP, APP_VDM_STAT_NAK, 0xFF01u, 1u);
    eq(APP_VDM_Get()->in_alt_mode, 0, "NAK enter: not in alt mode");
    eq(APP_VDM_Get()->n_enter_nak, 1, "NAK counted");
    eq(APP_VDM_Get()->pending, 0, "latch cleared by response");

    /* BUSY likewise leaves us out. */
    APP_VDM_ModeEnter(0u, APP_VDM_SOP, 0xFF01u, 1u);
    APP_VDM_OnModeEnter(0u, APP_VDM_SOP, APP_VDM_STAT_BUSY, 0xFF01u, 1u);
    eq(APP_VDM_Get()->in_alt_mode, 0, "BUSY enter: not in alt mode");
    eq(APP_VDM_Get()->n_enter_busy, 1, "BUSY counted");

    /* An ACK'd enter is what actually puts us in the mode. */
    APP_VDM_ModeEnter(0u, APP_VDM_SOP, 0xFF01u, 1u);
    APP_VDM_OnModeEnter(0u, APP_VDM_SOP, APP_VDM_STAT_ACK, 0xFF01u, 1u);
    eq(APP_VDM_Get()->in_alt_mode, 1, "ACK enter: in alt mode");
    eq(APP_VDM_Get()->n_enter_ack, 1, "ACK counted");
    eq(APP_VDM_Get()->last_svid, 0xFF01, "last svid recorded");
    eq(APP_VDM_Get()->last_mode_index, 1, "last mode index recorded");
    eq(APP_VDM_Get()->last_status, APP_VDM_STAT_ACK, "last status recorded");

    /* A NAK'd exit must leave us IN the mode. */
    APP_VDM_ModeExit(0u, APP_VDM_SOP, 0xFF01u, 1u);
    APP_VDM_OnModeExit(0u, APP_VDM_SOP, APP_VDM_STAT_NAK, 0xFF01u, 1u);
    eq(APP_VDM_Get()->in_alt_mode, 1, "NAK exit: still in alt mode");
    eq(APP_VDM_Get()->n_exit_nak, 1, "exit NAK counted");

    /* ACK'd exit leaves it. */
    APP_VDM_ModeExit(0u, APP_VDM_SOP, 0xFF01u, 1u);
    APP_VDM_OnModeExit(0u, APP_VDM_SOP, APP_VDM_STAT_ACK, 0xFF01u, 1u);
    eq(APP_VDM_Get()->in_alt_mode, 0, "ACK exit: out of alt mode");
    eq(APP_VDM_Get()->n_exit_ack, 1, "exit ACK counted");

    /* An unsolicited exit response (no request) must still be safe. */
    APP_VDM_OnModeExit(0u, APP_VDM_SOP, APP_VDM_STAT_ACK, 0x1234u, 3u);
    eq(APP_VDM_Get()->in_alt_mode, 0, "unsolicited exit safe");

    /* Clear preserves the live mode state but resets the counters. */
    APP_VDM_ModeEnter(0u, APP_VDM_SOP, 0xFF01u, 1u);
    APP_VDM_OnModeEnter(0u, APP_VDM_SOP, APP_VDM_STAT_ACK, 0xFF01u, 1u);
    eq(APP_VDM_Get()->in_alt_mode, 1, "in alt mode before clear");
    APP_VDM_Clear();
    eq(APP_VDM_Get()->in_alt_mode, 1, "clear preserves mode state");
    eq(APP_VDM_Get()->n_enter_ack, 0, "clear resets counters");
}

static void test_names(void)
{
    printf("test_names\n");
    eq(APP_VDM_ReqName(APP_VDM_REQ_MODE_ENTER)[0], 'E', "req name");
    eq(APP_VDM_StatName(APP_VDM_STAT_ACK)[0], 'A', "stat name");
    eq(APP_VDM_StatName(99u)[0], '?', "unknown stat name");
    eq(APP_VDM_SopName(APP_VDM_SOP2)[0], 'S', "sop name");
    eq(APP_VDM_ReqName(APP_VDM_REQ_COUNT)[0], '?', "unknown req name");
}

int main(void)
{
    printf("=== vdm engine host tests ===\n");
    test_validation();
    test_pe_rejection();
    test_status_classifier();
    test_response_path();
    test_names();
    printf("=== %s ===\n", (s_fail == 0) ? "PASS" : "FAIL");
    return (s_fail == 0) ? 0 : 1;
}
