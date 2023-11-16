/*!
 * \file      sx126x.c
 *
 * \brief     SX126x driver implementation
 *
 * \copyright Revised BSD License, see section \ref LICENSE.
 *
 * \code
 *                ______                              _
 *               / _____)             _              | |
 *              ( (____  _____ ____ _| |_ _____  ____| |__
 *               \____ \| ___ |    (_   _) ___ |/ ___)  _ \
 *               _____) ) ____| | | || |_| ____( (___| | | |
 *              (______/|_____)_|_|_| \__)_____)\____)_| |_|
 *              (C)2013-2017 Semtech
 *
 * \endcode
 *
 * \author    Miguel Luis ( Semtech )
 *
 * \author    Gregory Cristian ( Semtech )
 */
/**
  ******************************************************************************
  * @file    sx126x.c
  * @author  MCD Application Team
  * @brief   driver sx126x
  ******************************************************************************
  * @attention
  *
  * <h2><center>&copy; Copyright (c) 2018 STMicroelectronics.
  * All rights reserved.</center></h2>
  *
  * This software component is licensed by ST under BSD 3-Clause license,
  * the "License"; You may not use this file except in compliance with the
  * License. You may obtain a copy of the License at:
  *                        opensource.org/licenses/BSD-3-Clause
  *
  ******************************************************************************
  */
  
/* Includes ------------------------------------------------------------------*/
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include "sys_debug.h"
#include "radio.h"
#include "radio_conf.h"
#include "sx126x.h"

/*!
 * \brief Radio registers definition
 */
typedef struct
{
    uint16_t      Addr;                             //!< The address of the register
    uint8_t       Value;                            //!< The value of the register
}RadioRegisters_t;

/*!
 * \brief Holds the internal operating mode of the radio
 */
static RadioOperatingModes_t OperatingMode;

/*!
 * \brief Stores the current packet type set in the radio
 */
static RadioPacketTypes_t PacketType;

/*!
 * \brief Stores the current packet header type set in the radio
 */
static volatile RadioLoRaPacketLengthsMode_t LoRaHeaderType;

/*!
 * \brief Stores the last frequency error measured on LoRa received packet
 */
volatile uint32_t FrequencyError = 0;

/*!
 * \brief Hold the status of the Image calibration
 */
static bool ImageCalibrated = false;

uint8_t xta_trim = 0x12;

/*
 * SX126x DIO IRQ callback functions prototype
 */

/*
 * Private functions prototypes
 */


/*!
 * \brief Send a command that write data to the radio
 *
 * \param [in]  opcode        Opcode of the command
 * \param [in]  buffer        Buffer to be send to the radio
 * \param [in]  size          Size of the buffer to send
 */
void SX126xWriteCommand( RadioCommands_t opcode, uint8_t *buffer, uint16_t size );

/*!
 * \brief Send a command that read data from the radio
 *
 * \param [in]  opcode        Opcode of the command
 * \param [out] buffer        Buffer holding data from the radio
 * \param [in]  size          Size of the buffer
 *
 * \retval status Return command radio status
 */
uint8_t SX126xReadCommand( RadioCommands_t opcode, uint8_t *buffer, uint16_t size );

/*
 * Public global variables
 */

/*!
 * Hardware DIO IRQ callback initialization
 */

void SX126xInit( DioIrqHandler dioIrq )
{
    Sx_Board_Reset( );

    Sx_Board_IoIrqInit( &dioIrq );

    Sx_Board_WakeUp( );
    SX126xSetStandby( STDBY_XOSC );//SX126xSetStandby( STDBY_RC );

    // Initialize TCXO control
    if (Sx_Board_IsTcxo()  == true )
    {
      CalibrationParams_t calibParam;

      SX126xSetDio3AsTcxoCtrl( TCXO_CTRL_1_7V, Sx_Board_GetWakeUpTime( ) << 6 ); // convert from ms to SX126x time base
      calibParam.Value = 0x7F;
      SX126xCalibrate( calibParam );
    }

    // Initialize RF switch control
    SX126xSetDio2AsRfSwitchCtrl( true );

    SX126xSetOperatingMode(MODE_STDBY_XOSC);//SX126xSetOperatingMode( MODE_STDBY_RC );
}

void SX126xFreqCorrection(uint8_t trim){
	xta_trim = trim;
}


RadioOperatingModes_t SX126xGetOperatingMode( void )
{
    return OperatingMode;
}

void SX126xSetOperatingMode( RadioOperatingModes_t mode )
{
    OperatingMode = mode;
}

void SX126xCheckDeviceReady( void )
{
    if( ( SX126xGetOperatingMode( ) == MODE_SLEEP ) || ( SX126xGetOperatingMode( ) == MODE_RX_DC ) )
    {
        Sx_Board_WakeUp( );
        // Switch is turned off when device is in sleep mode and turned on is all other modes
        Sx_Board_SetAntSw( RFSW_ON );
    }
    Sx_Board_WaitOnBusy( );
}

void SX126xSetPayload( uint8_t *payload, uint8_t size )
{
    SX126xWriteBuffer( 0x00, payload, size );
}

uint8_t SX126xGetPayload( uint8_t *buffer, uint8_t *size,  uint8_t maxSize )
{
    uint8_t offset = 0;

    SX126xGetRxBufferStatus( size, &offset );
    if( *size > maxSize )
    {
        return 1;
    }
    SX126xReadBuffer( offset, buffer, *size );
    return 0;
}

void SX126xSendPayload( uint8_t *payload, uint8_t size, uint32_t timeout )
{
    SX126xSetPayload( payload, size );
    SX126xSetTx( timeout );
}

uint8_t SX126xSetSyncWord( uint8_t *syncWord )
{
    SX126xWriteRegisters( REG_LR_SYNCWORDBASEADDRESS, syncWord, 8 );
    return 0;
}

uint32_t SX126xGetRandom( void )
{
    uint32_t number = 0;
    uint8_t regAnaLna = 0;
    uint8_t regAnaMixer = 0;

    regAnaLna = SX126xReadRegister( REG_ANA_LNA );
    SX126xWriteRegister( REG_ANA_LNA, regAnaLna & ~( 1 << 0 ) );

    regAnaMixer = SX126xReadRegister( REG_ANA_MIXER );
    SX126xWriteRegister( REG_ANA_MIXER, regAnaMixer & ~( 1 << 7 ) );

    // Set radio in continuous reception
    SX126xSetRx( 0xFFFFFF ); // Rx Continuous

    SX126xReadRegisters( RANDOM_NUMBER_GENERATORBASEADDR, ( uint8_t* )&number, 4 );

    SX126xSetStandby( STDBY_XOSC );//SX126xSetStandby( STDBY_RC );

    SX126xWriteRegister( REG_ANA_LNA, regAnaLna );
    SX126xWriteRegister( REG_ANA_MIXER, regAnaMixer );

    return number;
}

void SX126xSetSleep( SleepParams_t sleepConfig )
{
    Sx_Board_SetAntSw( RFSW_OFF );

    uint8_t value = ( ( ( uint8_t )sleepConfig.Fields.WarmStart << 2 ) |
                      ( ( uint8_t )sleepConfig.Fields.Reset << 1 ) |
                      ( ( uint8_t )sleepConfig.Fields.WakeUpRTC ) );
    SX126xWriteCommand( RADIO_SET_SLEEP, &value, 1 );
    SX126xSetOperatingMode( MODE_SLEEP );
}

void SX126xSetStandby( RadioStandbyModes_t standbyConfig )
{
    SX126xWriteCommand( RADIO_SET_STANDBY, ( uint8_t* )&standbyConfig, 1 );
    if( standbyConfig == STDBY_RC )
    {
        SX126xSetOperatingMode( MODE_STDBY_RC );
    }
    else
    {
        SX126xSetOperatingMode( MODE_STDBY_XOSC );
    }
}

void SX126xSetFs( void )
{
    SX126xWriteCommand( RADIO_SET_FS, 0, 0 );
    SX126xSetOperatingMode( MODE_FS );
}

void SX126xSetTx( uint32_t timeout )
{
    uint8_t buf[3];

    SX126xSetOperatingMode( MODE_TX );

    buf[0] = ( uint8_t )( ( timeout >> 16 ) & 0xFF );
    buf[1] = ( uint8_t )( ( timeout >> 8 ) & 0xFF );
    buf[2] = ( uint8_t )( timeout & 0xFF );

	SX126xWriteRegister(REG_XTA_TRIM, xta_trim);
	SX126xWriteRegister(REG_XTB_TRIM, xta_trim);

    SX126xWriteCommand( RADIO_SET_TX, buf, 3 );
}

void SX126xSetRx( uint32_t timeout )
{
    uint8_t buf[3];

    SX126xSetOperatingMode( MODE_RX );

    buf[0] = ( uint8_t )( ( timeout >> 16 ) & 0xFF );
    buf[1] = ( uint8_t )( ( timeout >> 8 ) & 0xFF );
    buf[2] = ( uint8_t )( timeout & 0xFF );

	SX126xWriteRegister(REG_XTA_TRIM, xta_trim);
	SX126xWriteRegister(REG_XTB_TRIM, xta_trim);

    SX126xWriteCommand( RADIO_SET_RX, buf, 3 );
}

void SX126xSetRxBoosted( uint32_t timeout )
{
    uint8_t buf[3];

    SX126xSetOperatingMode( MODE_RX );

    SX126xWriteRegister( REG_RX_GAIN, 0x96 ); // max LNA gain, increase current by ~2mA for around ~3dB in sensivity

    buf[0] = ( uint8_t )( ( timeout >> 16 ) & 0xFF );
    buf[1] = ( uint8_t )( ( timeout >> 8 ) & 0xFF );
    buf[2] = ( uint8_t )( timeout & 0xFF );
    SX126xWriteCommand( RADIO_SET_RX, buf, 3 );
}

void SX126xSetRxDutyCycle( uint32_t rxTime, uint32_t sleepTime )
{
    uint8_t buf[6];

    buf[0] = ( uint8_t )( ( rxTime >> 16 ) & 0xFF );
    buf[1] = ( uint8_t )( ( rxTime >> 8 ) & 0xFF );
    buf[2] = ( uint8_t )( rxTime & 0xFF );
    buf[3] = ( uint8_t )( ( sleepTime >> 16 ) & 0xFF );
    buf[4] = ( uint8_t )( ( sleepTime >> 8 ) & 0xFF );
    buf[5] = ( uint8_t )( sleepTime & 0xFF );
    SX126xWriteCommand( RADIO_SET_RXDUTYCYCLE, buf, 6 );
    SX126xSetOperatingMode( MODE_RX_DC );
}

void SX126xSetCad( void )
{
    SX126xWriteCommand( RADIO_SET_CAD, 0, 0 );
    SX126xSetOperatingMode( MODE_CAD );
}

void SX126xSetTxContinuousWave( void )
{
    SX126xWriteCommand( RADIO_SET_TXCONTINUOUSWAVE, 0, 0 );
    SX126xSetOperatingMode( MODE_TX );
}

void SX126xSetTxInfinitePreamble( void )
{
    SX126xWriteCommand( RADIO_SET_TXCONTINUOUSPREAMBLE, 0, 0 );
    SX126xSetOperatingMode( MODE_TX );
}

void SX126xSetStopRxTimerOnPreambleDetect( bool enable )
{
    SX126xWriteCommand( RADIO_SET_STOPRXTIMERONPREAMBLE, ( uint8_t* )&enable, 1 );
}

void SX126xSetLoRaSymbNumTimeout( uint8_t symbNum )
{
    SX126xWriteCommand( RADIO_SET_LORASYMBTIMEOUT, &symbNum, 1 );

    if( symbNum >= 64 )
    {
        uint8_t mant = symbNum >> 1;
        uint8_t exp  = 0;
        uint8_t reg  = 0;

        while( mant > 31 )
        {
            mant >>= 2;
            exp++;
        }

        reg = exp + ( mant << 3 );
        SX126xWriteRegister( REG_LR_SYNCH_TIMEOUT, reg );
    }
}

void SX126xSetRegulatorMode( RadioRegulatorMode_t mode )
{
    SX126xWriteCommand( RADIO_SET_REGULATORMODE, ( uint8_t* )&mode, 1 );
}

void SX126xCalibrate( CalibrationParams_t calibParam )
{
    uint8_t value = ( ( ( uint8_t )calibParam.Fields.ImgEnable << 6 ) |
                      ( ( uint8_t )calibParam.Fields.ADCBulkPEnable << 5 ) |
                      ( ( uint8_t )calibParam.Fields.ADCBulkNEnable << 4 ) |
                      ( ( uint8_t )calibParam.Fields.ADCPulseEnable << 3 ) |
                      ( ( uint8_t )calibParam.Fields.PLLEnable << 2 ) |
                      ( ( uint8_t )calibParam.Fields.RC13MEnable << 1 ) |
                      ( ( uint8_t )calibParam.Fields.RC64KEnable ) );

    SX126xWriteCommand( RADIO_CALIBRATE, &value, 1 );
}

void SX126xCalibrateImage( uint32_t freq )
{
    uint8_t calFreq[2];

    if( freq > 900000000 )
    {
        calFreq[0] = 0xE1;
        calFreq[1] = 0xE9;
    }
    else if( freq > 850000000 )
    {
        calFreq[0] = 0xD7;
        calFreq[1] = 0xDB;
    }
    else if( freq > 770000000 )
    {
        calFreq[0] = 0xC1;
        calFreq[1] = 0xC5;
    }
    else if( freq > 460000000 )
    {
        calFreq[0] = 0x75;
        calFreq[1] = 0x81;
    }
    else if( freq > 425000000 )
    {
        calFreq[0] = 0x6B;
        calFreq[1] = 0x6F;
    }
    SX126xWriteCommand( RADIO_CALIBRATEIMAGE, calFreq, 2 );
}

void SX126xSetPaConfig( uint8_t paDutyCycle, uint8_t hpMax, uint8_t deviceSel, uint8_t paLut )
{
    uint8_t buf[4];

    buf[0] = paDutyCycle;
    buf[1] = hpMax;
    buf[2] = deviceSel;
    buf[3] = paLut;
    SX126xWriteCommand( RADIO_SET_PACONFIG, buf, 4 );
}

void SX126xSetRxTxFallbackMode( uint8_t fallbackMode )
{
    SX126xWriteCommand( RADIO_SET_TXFALLBACKMODE, &fallbackMode, 1 );
}

void SX126xSetDioIrqParams( uint16_t irqMask, uint16_t dio1Mask, uint16_t dio2Mask, uint16_t dio3Mask )
{
    uint8_t buf[8];

    buf[0] = ( uint8_t )( ( irqMask >> 8 ) & 0x00FF );
    buf[1] = ( uint8_t )( irqMask & 0x00FF );
    buf[2] = ( uint8_t )( ( dio1Mask >> 8 ) & 0x00FF );
    buf[3] = ( uint8_t )( dio1Mask & 0x00FF );
    buf[4] = ( uint8_t )( ( dio2Mask >> 8 ) & 0x00FF );
    buf[5] = ( uint8_t )( dio2Mask & 0x00FF );
    buf[6] = ( uint8_t )( ( dio3Mask >> 8 ) & 0x00FF );
    buf[7] = ( uint8_t )( dio3Mask & 0x00FF );
    SX126xWriteCommand( RADIO_CFG_DIOIRQ, buf, 8 );
}

uint16_t SX126xGetIrqStatus( void )
{
    uint8_t irqStatus[2];

    SX126xReadCommand( RADIO_GET_IRQSTATUS, irqStatus, 2 );
    return ( irqStatus[0] << 8 ) | irqStatus[1];
}

void SX126xSetDio2AsRfSwitchCtrl( uint8_t enable )
{
    SX126xWriteCommand( RADIO_SET_RFSWITCHMODE, &enable, 1 );
}

void SX126xSetDio3AsTcxoCtrl( RadioTcxoCtrlVoltage_t tcxoVoltage, uint32_t timeout )
{
    uint8_t buf[4];

    buf[0] = tcxoVoltage & 0x07;
    buf[1] = ( uint8_t )( ( timeout >> 16 ) & 0xFF );
    buf[2] = ( uint8_t )( ( timeout >> 8 ) & 0xFF );
    buf[3] = ( uint8_t )( timeout & 0xFF );

    SX126xWriteCommand( RADIO_SET_TCXOMODE, buf, 4 );
}

void SX126xSetRfFrequency( uint32_t frequency )
{
    uint8_t buf[4];
    uint32_t freq = 0;

    if( ImageCalibrated == false )
    {
        SX126xCalibrateImage( frequency );
        ImageCalibrated = true;
    }

    freq = ( uint32_t )( ( double )frequency / ( double )FREQ_STEP );
    buf[0] = ( uint8_t )( ( freq >> 24 ) & 0xFF );
    buf[1] = ( uint8_t )( ( freq >> 16 ) & 0xFF );
    buf[2] = ( uint8_t )( ( freq >> 8 ) & 0xFF );
    buf[3] = ( uint8_t )( freq & 0xFF );
    SX126xWriteCommand( RADIO_SET_RFFREQUENCY, buf, 4 );
}

void SX126xSetPacketType( RadioPacketTypes_t packetType )
{
    // Save packet type internally to avoid questioning the radio
    PacketType = packetType;
    SX126xWriteCommand( RADIO_SET_PACKETTYPE, ( uint8_t* )&packetType, 1 );
}

RadioPacketTypes_t SX126xGetPacketType( void )
{
    return PacketType;
}

void SX126xSetRfTxPower( int8_t power )
{
    SX126xSetTxParams( power, RADIO_RAMP_40_US );
}

void SX126xSetTxParams( int8_t power, RadioRampTimes_t rampTime )
{
    uint8_t buf[2];

	// WORKAROUND - Better Resistance of the SX1262 Tx to Antenna Mismatch, see DS_SX1261-2_V1.2 datasheet chapter 15.2
	// RegTxClampConfig = @address 0x08D8
	SX126xWriteRegister( 0x08D8, SX126xReadRegister( 0x08D8 ) | ( 0x0F << 1 ) );
	// WORKAROUND END

	SX126xSetPaConfig( 0x04, 0x07, 0x00, 0x01 );
	if( power > 22 )
	{
		power = 22;
	}
	else if( power < -9 )
	{
		power = -9;
	}
    buf[0] = power;
    buf[1] = ( uint8_t )rampTime;
    SX126xWriteCommand( RADIO_SET_TXPARAMS, buf, 2 );
}

void SX126xSetModulationParams( ModulationParams_t *modulationParams )
{
    uint8_t n;
    uint8_t buf[8] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };

    // Check if required configuration corresponds to the stored packet type
    // If not, silently update radio packet type
    if( PacketType != modulationParams->PacketType )
    {
        SX126xSetPacketType( modulationParams->PacketType );
    }

    if( modulationParams->PacketType == PACKET_TYPE_LORA)
    {
        n = 4;
        buf[0] = modulationParams->Params.LoRa.SpreadingFactor;
        buf[1] = modulationParams->Params.LoRa.Bandwidth;
        buf[2] = modulationParams->Params.LoRa.CodingRate;
        buf[3] = modulationParams->Params.LoRa.LowDatarateOptimize;

        SX126xWriteCommand( RADIO_SET_MODULATIONPARAMS, buf, n );

    }
}

void SX126xSetPacketParams( PacketParams_t *packetParams )
{
    uint8_t n;
    uint8_t buf[9] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };

    // Check if required configuration corresponds to the stored packet type
    // If not, silently update radio packet type
    if( PacketType != packetParams->PacketType )
    {
        SX126xSetPacketType( packetParams->PacketType );
    }

    if( packetParams->PacketType == PACKET_TYPE_LORA)
    {
        n = 6;
        buf[0] = ( packetParams->Params.LoRa.PreambleLength >> 8 ) & 0xFF;
        buf[1] = packetParams->Params.LoRa.PreambleLength;
        buf[2] = LoRaHeaderType = packetParams->Params.LoRa.HeaderType;
        buf[3] = packetParams->Params.LoRa.PayloadLength;
        buf[4] = packetParams->Params.LoRa.CrcMode;
        buf[5] = packetParams->Params.LoRa.InvertIQ;
    }
    SX126xWriteCommand( RADIO_SET_PACKETPARAMS, buf, n );
}

void SX126xSetCadParams( RadioLoRaCadSymbols_t cadSymbolNum, uint8_t cadDetPeak, uint8_t cadDetMin, RadioCadExitModes_t cadExitMode, uint32_t cadTimeout )
{
    uint8_t buf[7];

    buf[0] = ( uint8_t )cadSymbolNum;
    buf[1] = cadDetPeak;
    buf[2] = cadDetMin;
    buf[3] = ( uint8_t )cadExitMode;
    buf[4] = ( uint8_t )( ( cadTimeout >> 16 ) & 0xFF );
    buf[5] = ( uint8_t )( ( cadTimeout >> 8 ) & 0xFF );
    buf[6] = ( uint8_t )( cadTimeout & 0xFF );
    SX126xWriteCommand( RADIO_SET_CADPARAMS, buf, 7 );
    SX126xSetOperatingMode( MODE_CAD );
}

void SX126xSetBufferBaseAddress( uint8_t txBaseAddress, uint8_t rxBaseAddress )
{
    uint8_t buf[2];

    buf[0] = txBaseAddress;
    buf[1] = rxBaseAddress;
    SX126xWriteCommand( RADIO_SET_BUFFERBASEADDRESS, buf, 2 );
}

RadioStatus_t SX126xGetStatus( void )
{
    uint8_t stat = 0;
    RadioStatus_t status = { .Value = 0 };

    stat = SX126xReadCommand( RADIO_GET_STATUS, NULL, 0 );
    status.Fields.CmdStatus = ( stat & ( 0x07 << 1 ) ) >> 1;
    status.Fields.ChipMode = ( stat & ( 0x07 << 4 ) ) >> 4;
    return status;
}

int8_t SX126xGetRssiInst( void )
{
    uint8_t buf[1];
    int8_t rssi = 0;

    SX126xReadCommand( RADIO_GET_RSSIINST, buf, 1 );
    rssi = -buf[0] >> 1;
    return rssi;
}

void SX126xGetRxBufferStatus( uint8_t *payloadLength, uint8_t *rxStartBufferPointer )
{
    uint8_t status[2];

    SX126xReadCommand( RADIO_GET_RXBUFFERSTATUS, status, 2 );

    // In case of LORA fixed header, the payloadLength is obtained by reading
    // the register REG_LR_PAYLOADLENGTH
    if( ( SX126xGetPacketType( ) == PACKET_TYPE_LORA ) && ( LoRaHeaderType == LORA_PACKET_FIXED_LENGTH ) )
    {
        *payloadLength = SX126xReadRegister( REG_LR_PAYLOADLENGTH );
    }
    else
    {
        *payloadLength = status[0];
    }
    *rxStartBufferPointer = status[1];
}

void SX126xGetPacketStatus( PacketStatus_t *pktStatus )
{
    uint8_t status[3];

    SX126xReadCommand( RADIO_GET_PACKETSTATUS, status, 3 );

    pktStatus->packetType = SX126xGetPacketType( );
    if( pktStatus->packetType == PACKET_TYPE_LORA)
    {
            pktStatus->Params.LoRa.RssiPkt = -status[0] >> 1;
            // Returns SNR value [dB] rounded to the nearest integer value
            pktStatus->Params.LoRa.SnrPkt = ( ( ( int8_t )status[1] ) + 2 ) >> 2;
            pktStatus->Params.LoRa.SignalRssiPkt = -status[2] >> 1;
            pktStatus->Params.LoRa.FreqError = FrequencyError;
    }else if(pktStatus->packetType == PACKET_TYPE_NONE){
            // In that specific case, we set everything in the pktStatus to zeros
            // and reset the packet type accordingly
            memset( pktStatus, 0, sizeof( PacketStatus_t ) );
            pktStatus->packetType = PACKET_TYPE_NONE;
    }
}

RadioError_t SX126xGetDeviceErrors( void )
{
    uint8_t err[] = { 0, 0 };
    RadioError_t error = { .Value = 0 };

    SX126xReadCommand( RADIO_GET_ERROR, ( uint8_t* )err, 2 );
    error.Fields.PaRamp     = ( err[0] & ( 1 << 0 ) ) >> 0;
    error.Fields.PllLock    = ( err[1] & ( 1 << 6 ) ) >> 6;
    error.Fields.XoscStart  = ( err[1] & ( 1 << 5 ) ) >> 5;
    error.Fields.ImgCalib   = ( err[1] & ( 1 << 4 ) ) >> 4;
    error.Fields.AdcCalib   = ( err[1] & ( 1 << 3 ) ) >> 3;
    error.Fields.PllCalib   = ( err[1] & ( 1 << 2 ) ) >> 2;
    error.Fields.Rc13mCalib = ( err[1] & ( 1 << 1 ) ) >> 1;
    error.Fields.Rc64kCalib = ( err[1] & ( 1 << 0 ) ) >> 0;
    return error;
}

void SX126xClearDeviceErrors( void )
{
    uint8_t buf[2] = { 0x00, 0x00 };
    SX126xWriteCommand( RADIO_CLR_ERROR, buf, 2 );
}

void SX126xClearIrqStatus( uint16_t irq )
{
    uint8_t buf[2];

    buf[0] = ( uint8_t )( ( ( uint16_t )irq >> 8 ) & 0x00FF );
    buf[1] = ( uint8_t )( ( uint16_t )irq & 0x00FF );
    SX126xWriteCommand( RADIO_CLR_IRQSTATUS, buf, 2 );
}

void SX126xWriteCommand( RadioCommands_t command, uint8_t *buffer, uint16_t size )
{
    SX126xCheckDeviceReady( );

    Sx_Board_ChipSelect( 0 );

    Sx_Board_SendRecv(  ( uint8_t )command );

    for( uint16_t i = 0; i < size; i++ )
    {
        Sx_Board_SendRecv(  buffer[i] );
    }

    Sx_Board_ChipSelect( 1 );

    if( command != RADIO_SET_SLEEP )
    {
        Sx_Board_WaitOnBusy( );
    }
}

uint8_t SX126xReadCommand( RadioCommands_t command, uint8_t *buffer, uint16_t size )
{
    uint8_t status = 0;

    SX126xCheckDeviceReady( );

    Sx_Board_ChipSelect( 0 );

    Sx_Board_SendRecv(  ( uint8_t )command );
    status = Sx_Board_SendRecv(  0x00 );
    for( uint16_t i = 0; i < size; i++ )
    {
        buffer[i] = Sx_Board_SendRecv(  0 );
    }

    Sx_Board_ChipSelect( 1 );

    Sx_Board_WaitOnBusy( );

    return status;
}

void SX126xWriteRegisters( uint16_t address, uint8_t *buffer, uint16_t size )
{
    SX126xCheckDeviceReady( );

    Sx_Board_ChipSelect( 0 );
    
    Sx_Board_SendRecv(  RADIO_WRITE_REGISTER );
    Sx_Board_SendRecv(  ( address & 0xFF00 ) >> 8 );
    Sx_Board_SendRecv(  address & 0x00FF );
    
    for( uint16_t i = 0; i < size; i++ )
    {
        Sx_Board_SendRecv(  buffer[i] );
    }

    Sx_Board_ChipSelect( 1 );

    Sx_Board_WaitOnBusy( );
}

void SX126xWriteRegister( uint16_t address, uint8_t value )
{
    SX126xWriteRegisters( address, &value, 1 );
}

void SX126xReadRegisters( uint16_t address, uint8_t *buffer, uint16_t size )
{
    SX126xCheckDeviceReady( );

    Sx_Board_ChipSelect( 0 );

    Sx_Board_SendRecv(  RADIO_READ_REGISTER );
    Sx_Board_SendRecv(  ( address & 0xFF00 ) >> 8 );
    Sx_Board_SendRecv(  address & 0x00FF );
    Sx_Board_SendRecv(  0 );
    for( uint16_t i = 0; i < size; i++ )
    {
        buffer[i] = Sx_Board_SendRecv(  0 );
    }
    Sx_Board_ChipSelect( 1 );

    Sx_Board_WaitOnBusy( );
}

uint8_t SX126xReadRegister( uint16_t address )
{
    uint8_t data;
    SX126xReadRegisters( address, &data, 1 );
    return data;
}

void SX126xWriteBuffer( uint8_t offset, uint8_t *buffer, uint8_t size )
{
    SX126xCheckDeviceReady( );

    Sx_Board_ChipSelect( 0 );

    Sx_Board_SendRecv(  RADIO_WRITE_BUFFER );
    Sx_Board_SendRecv(  offset );
    for( uint16_t i = 0; i < size; i++ )
    {
        Sx_Board_SendRecv(  buffer[i] );
    }
    Sx_Board_ChipSelect( 1 );

    Sx_Board_WaitOnBusy( );
}

void SX126xReadBuffer( uint8_t offset, uint8_t *buffer, uint8_t size )
{
    SX126xCheckDeviceReady( );

    Sx_Board_ChipSelect( 0 );

    Sx_Board_SendRecv(  RADIO_READ_BUFFER );
    Sx_Board_SendRecv(  offset );
    Sx_Board_SendRecv(  0 );
    for( uint16_t i = 0; i < size; i++ )
    {
        buffer[i] = Sx_Board_SendRecv(  0 );
    }
    Sx_Board_ChipSelect( 1 );

    Sx_Board_WaitOnBusy( );
}

/************************ (C) COPYRIGHT STMicroelectronics *****END OF FILE****/

