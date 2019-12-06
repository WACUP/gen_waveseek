#ifndef NULLSOFT_API_H
#define NULLSOFT_API_H

#include <api/service/api_service.h>
extern api_service *serviceManager;
#define WASABI_API_SVC serviceManager

#include <api/application/api_application.h>
#define WASABI_API_APP applicationApi

#include <api/service/waServiceFactory.h>

#include <Agave/Language/api_language.h>

#include <Agave/DecodeFile/api_decodefile2.h>
extern api_decodefile2 *decodeFile2;
#define WASABI_API_DECODEFILE2 decodeFile2

#include <loader/hook/api_skin.h>
#define WASABI_API_SKIN skinApi

#endif