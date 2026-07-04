from django.urls import path

from . import views

urlpatterns = [
    path("logs", views.logs),
    path("login", views.login),
    path("stats", views.stats),
    path("health", views.health),
]
