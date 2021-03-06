/*
    ChibiOS/RT - Copyright (C) 2006,2007,2008,2009,2010,
                 2011,2012 Giovanni Di Sirio.

    This file is part of ChibiOS/RT.

    ChibiOS/RT is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 3 of the License, or
    (at your option) any later version.

    ChibiOS/RT is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

/**
 * @file    BCM2835/i2c_lld.c
 * @brief   I2C Driver subsystem low level driver source template.
 *
 * @addtogroup I2C
 * @{
 */

#include "ch.h"
#include "hal.h"

#if HAL_USE_I2C || defined(__DOXYGEN__)

/*===========================================================================*/
/* Driver local definitions.                                                 */
/*===========================================================================*/

/*===========================================================================*/
/* Driver exported variables.                                                */
/*===========================================================================*/

// TODO: What board uses BSC0 as its default/exposed I2C controller?
//   (It seems to be what Steve Bate's was using.)
/**
 * @brief   Driver for the I2C (BSC) controller numbered 0 (zero) in the datasheet for the BCM2835.
 *
 * @details Enabled by defining BCM2835_I2C_USE_I2C0 to 1, or by defining
 *          HAL_USE_I2C to 1 and including a board configuration that
 *          uses this controller as its default I2C controller.
 */
#if BCM2835_I2C_BSC0_ENABLED_ || defined(__DOXYGEN__)
I2CDriver I2CD0;
#endif

/**
 * @brief   Driver for the I2C (BSC) controller numbered 1 (one) in the datasheet for the BCM2835.
 *
 * @details Enabled by defining BCM2835_I2C_USE_I2C1 to 1, or by defining
 *          HAL_USE_I2C to 1 and including a board configuration that
 *          uses this controller as its default I2C controller.
 */
#if BCM2835_I2C_BSC1_ENABLED_ || defined(__DOXYGEN__)
I2CDriver I2CD1;
#endif

/**
 * @brief   Driver for the I2C (BSC) controller numbered 1 (one) in the datasheet for the BCM2835.
 *
 * @details Enabled by defining BCM2835_I2C_USE_I2C2 to 1.
 *
 *          The BCM2835 "Peripherals" datasheet does not mention any GPIO pins
 *          for this controller. This controller will be completely inaccessible,
 *          as there are no pins for i2cStart to configure as I2C pins when
 *          attempting to start this controller. This controller is, nonetheless,
 *          made available, just in case someone knows what to do with it.
 */
#if BCM2835_I2C_BSC2_ENABLED_ || defined(__DOXYGEN__)
I2CDriver I2CD2;
#endif

/*===========================================================================*/
/* Driver local variables.                                                   */
/*===========================================================================*/

/*===========================================================================*/
/* Driver local functions.                                                   */
/*===========================================================================*/

/**
 * @brief   Wakes up the waiting thread.
 *
 * @param[in] i2cp      pointer to the @p I2CDriver object
 * @param[in] msg       wakeup message
 *
 * @notapi
 */
#define wakeup_isr(i2cp, msg) {                                             \
  chSysLockFromIsr();                                                       \
  if ((i2cp)->thread != NULL) {                                             \
    Thread *tp = (i2cp)->thread;                                            \
    (i2cp)->thread = NULL;                                                  \
    tp->p_u.rdymsg = (msg);                                                 \
    chSchReadyI(tp);                                                        \
  }                                                                         \
  chSysUnlockFromIsr();                                                     \
}

/**
 * @brief   Handling of stalled I2C transactions.
 *
 * @param[in] i2cp      pointer to the @p I2CDriver object
 *
 * @notapi
 */
static void i2c_lld_safety_timeout(void *p) {
  I2CDriver *i2cp = (I2CDriver *)p;
  chSysLockFromIsr();
  if (i2cp->thread) {
    bscdevice_t *device = i2cp->device;

    i2cp->errors |= I2CD_TIMEOUT;
    if (device->status & BSC_CLKT)
      i2cp->errors |= I2CD_BUS_ERROR;
    if (device->status & BSC_ERR)
      i2cp->errors |= I2CD_ACK_FAILURE;

    device->control = 0;
    device->status = BSC_CLKT | BSC_ERR | BSC_DONE;

    Thread *tp = i2cp->thread;
    i2cp->thread = NULL;
    tp->p_u.rdymsg = RDY_TIMEOUT;
    chSchReadyI(tp);
  }
  chSysUnlockFromIsr();
}

/*===========================================================================*/
/* Driver interrupt handlers.                                                */
/*===========================================================================*/

void i2c_lld_serve_interrupt(I2CDriver *i2cp) {
  UNUSED(i2cp);
  bscdevice_t *device = i2cp->device;
  uint32_t status = device->status;

  if (status & (BSC_CLKT | BSC_ERR)) {
    // TODO: Do other status flags combine with these to indicate different actual causes?
    i2cp->errors = I2CD_NO_ERROR;
    if (status & BSC_CLKT)
      i2cp->errors |= I2CD_TIMEOUT;
    if (status & BSC_ERR)
      i2cp->errors |= I2CD_ACK_FAILURE;

    wakeup_isr(i2cp, RDY_RESET);
  }
  else if (status & BSC_DONE) {
    while ((status & BSC_RXD) && (i2cp->rxidx < i2cp->rxbytes))
      i2cp->rxbuf[i2cp->rxidx++] = device->dataFifo;
    device->control = 0;
    device->status = BSC_CLKT | BSC_ERR | BSC_DONE;
    wakeup_isr(i2cp, RDY_OK);
  }
  else if (status & BSC_TXW) {
    while ((i2cp->txidx < i2cp->txbytes) && (status & BSC_TXD))
      device->dataFifo = i2cp->txbuf[i2cp->txidx++];
  }
  else if (status & BSC_RXR) {
    while ((i2cp->rxidx < i2cp->rxbytes) && (status & BSC_RXD))
      i2cp->rxbuf[i2cp->rxidx++] = device->dataFifo;
  }
}

/*===========================================================================*/
/* Driver exported functions.                                                */
/*===========================================================================*/

/**
 * @brief   Low level I2C driver initialization.
 *
 * @notapi
 */
void i2c_lld_init(void) {
#if BCM2835_I2C_BSC0_ENABLED_
  I2CD0.device = BSC0_ADDR;
  i2cObjectInit(&I2CD0);
#endif

#if BCM2835_I2C_BSC1_ENABLED_
  I2CD1.device = BSC1_ADDR;
  i2cObjectInit(&I2CD1);
#endif

#if BCM2835_I2C_BSC2_ENABLED_
  I2CD2.device = BSC2_ADDR;
  i2cObjectInit(&I2CD2);
#endif
}

/**
 * @brief   Configures and activates the I2C peripheral.
 *
 * @param[in] i2cp      pointer to the @p I2CDriver object
 *
 * @notapi
 */
void i2c_lld_start(I2CDriver *i2cp) {

  /* Set up GPIO pins for I2C */
#if BCM2835_I2C_BSC0_ENABLED_
  if ( i2cp->device == BSC0_ADDR ) {
    bcm2835_gpio_fnsel(GPIO0_PAD, GPFN_ALT0);
    bcm2835_gpio_fnsel(GPIO1_PAD, GPFN_ALT0);
  }
#endif

#if BCM2835_I2C_BSC1_ENABLED_
  if ( i2cp->device == BSC1_ADDR ) {
    bcm2835_gpio_fnsel(GPIO2_PAD, GPFN_ALT0);
    bcm2835_gpio_fnsel(GPIO3_PAD, GPFN_ALT0);
  }
#endif

  // The I2C #2 controller (BSC2) doesn't seem to have any GPIO pins
  // associated with it. This means we can't enable it.

  uint32_t speed = i2cp->config->ic_speed;
  if (speed != 0 && speed != 100000)
    i2cp->device->clockDivider = BSC_CLOCK_FREQ / i2cp->config->ic_speed;

  i2cp->device->control |= BSC_I2CEN;
}

/**
 * @brief   Deactivates the I2C peripheral.
 *
 * @param[in] i2cp      pointer to the @p I2CDriver object
 *
 * @notapi
 */
void i2c_lld_stop(I2CDriver *i2cp) {
  /* Set GPIO pin function to default */
#if BCM2835_I2C_BSC0_ENABLED_
  if ( i2cp->device == BSC0_ADDR ) {
    bcm2835_gpio_fnsel(GPIO0_PAD, GPFN_IN);
    bcm2835_gpio_fnsel(GPIO1_PAD, GPFN_IN);
  }
#endif

#if BCM2835_I2C_BSC1_ENABLED_
  if ( i2cp->device == BSC1_ADDR ) {
    bcm2835_gpio_fnsel(GPIO2_PAD, GPFN_IN);
    bcm2835_gpio_fnsel(GPIO3_PAD, GPFN_IN);
  }
#endif

  i2cp->device->control &= ~BSC_I2CEN;
}

/**
 * @brief   Master transmission.
 *
 * @param[in] i2cp      pointer to the @p I2CDriver object
 * @param[in] addr      slave device address (7 bits) without R/W bit
 * @param[in] txbuf     transmit data buffer pointer
 * @param[in] txbytes   number of bytes to be transmitted
 * @param[out] rxbuf     receive data buffer pointer
 * @param[in] rxbytes   number of bytes to be received
 * @param[in] timeout   the number of ticks before the operation timeouts,
 *                      the following special values are allowed:
 *                      - @a TIME_INFINITE no timeout.
 *                      .
 *
 * @notapi
 */
msg_t i2c_lld_master_transmit_timeout(I2CDriver *i2cp, i2caddr_t addr, 
                                       const uint8_t *txbuf, size_t txbytes, 
                                       uint8_t *rxbuf, const uint8_t rxbytes, 
                                       systime_t timeout) {
  VirtualTimer vt;

  /* Global timeout for the whole operation.*/
  if (timeout != TIME_INFINITE)
    chVTSetI(&vt, timeout, i2c_lld_safety_timeout, (void *)i2cp);

  i2cp->addr = addr;
  i2cp->txbuf = txbuf;
  i2cp->txbytes = txbytes;
  i2cp->txidx = 0;
  i2cp->rxbuf = rxbuf;
  i2cp->rxbytes = rxbytes;
  i2cp->rxidx = 0;

  bscdevice_t *device = i2cp->device;
  device->slaveAddress = addr;
  device->dataLength = txbytes;
  device->status = CLEAR_STATUS;

  /* Enable Interrupts and start transfer.*/
  device->control |= (BSC_INTT | BSC_INTD | START_WRITE);

  /* Is this really needed? there is an outer lock already */
  chSysLock();

  i2cp->thread = chThdSelf();
  chSchGoSleepS(THD_STATE_SUSPENDED);
  if ((timeout != TIME_INFINITE) && chVTIsArmedI(&vt))
    chVTResetI(&vt);

  chSysUnlock();

  msg_t status = chThdSelf()->p_u.rdymsg;

  if (status == RDY_OK && rxbytes > 0) {
    /* The TIMEOUT_INFINITE prevents receive from setting up it's own timer.*/
    status = i2c_lld_master_receive_timeout(i2cp, addr, rxbuf, 
					    rxbytes, TIME_INFINITE);
    if ((timeout != TIME_INFINITE) && chVTIsArmedI(&vt))
      chVTResetI(&vt);
  }

  return status;
}


/**
 * @brief   Master receive.
 *
 * @param[in] i2cp      pointer to the @p I2CDriver object
 * @param[in] addr      slave device address (7 bits) without R/W bit
 * @param[out] rxbuf     receive data buffer pointer
 * @param[in] rxbytes   number of bytes to be received
 * @param[in] timeout   the number of ticks before the operation timeouts,
 *                      the following special values are allowed:
 *                      - @a TIME_INFINITE no timeout.
 *                      .
 *
 * @notapi
 */
msg_t i2c_lld_master_receive_timeout(I2CDriver *i2cp, i2caddr_t addr, 
				     uint8_t *rxbuf, size_t rxbytes, 
				     systime_t timeout) {
  VirtualTimer vt;

  /* Global timeout for the whole operation.*/
  if (timeout != TIME_INFINITE)
    chVTSetI(&vt, timeout, i2c_lld_safety_timeout, (void *)i2cp);

  i2cp->addr = addr;
  i2cp->txbuf = NULL;
  i2cp->txbytes = 0;
  i2cp->txidx = 0;
  i2cp->rxbuf = rxbuf;
  i2cp->rxbytes = rxbytes;
  i2cp->rxidx = 0;

  /* Setup device.*/
  bscdevice_t *device = i2cp->device;
  device->slaveAddress = addr;
  device->dataLength = rxbytes;
  device->status = CLEAR_STATUS;

  /* Enable Interrupts and start transfer.*/
  device->control = (BSC_INTR | BSC_INTD | START_READ);

  // needed? there is an outer lock already
  chSysLock();
  i2cp->thread = chThdSelf();
  chSchGoSleepS(THD_STATE_SUSPENDED);
  if ((timeout != TIME_INFINITE) && chVTIsArmedI(&vt))
    chVTResetI(&vt);
  chSysUnlock();

  return chThdSelf()->p_u.rdymsg;
}

#endif /* HAL_USE_I2C */

/** @} */
